// gs_hud.c - see gs_hud.h.

#include "ui/gs_hud.h"

#include "dcimgui.h"
#include "gfx/gs_render.h"
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

// The condition bar, which is a shape rather than text and so has a height of
// its own rather than one worked out from a line.
#define GS_HUD_BAR 8.0f

// How much bigger than the body text the two kinds of number are drawn.
#define GS_HUD_BIG   2.2f
#define GS_HUD_SMALL 1.5f

// A number and what it is, the number large and the label small under it. The
// eye reads the number at a glance and only reads the label the first time,
// which is what the whole thing has to survive: it is looked at while driving.
// **How much of its natural size this panel is being drawn at.** One for a HUD
// with room for itself, less than one in a view too small for it - see
// gs_hud_draw. Everything that sets a font scale multiplies by it, because
// ImGui's window font scale is absolute rather than relative and a row setting
// its own would otherwise undo the panel's.
static float gs_hud_zoom = 1.0f;

static void gs_hud_stat(const char *label, const char *value, float scale) {
    float x = ImGui_GetCursorPosX();

    ImGui_SetWindowFontScale(scale * gs_hud_zoom);
    ImGui_TextUnformatted(value);
    ImGui_SetWindowFontScale(gs_hud_zoom);

    ImGui_SetCursorPosX(x);
    float r, g, b;
    gs_style_accent(&r, &g, &b);
    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ r, g, b, 0.85f });
    ImGui_TextUnformatted(label);
    ImGui_PopStyleColor();
}

// What did not fit in the panel last time it was drawn - see gs_hud_overflow.
static float gs_hud_hidden = 0.0f;
static float gs_hud_room;

// And what the carrying row said - see gs_hud_carrying.
static char gs_hud_carried[32];

// The damage bar. A number from 0 to 255 means nothing to a driver; a bar that
// is running out means something immediately, and the colour says how worried to
// be without anybody having to read it.
static void gs_hud_damage(const gs_car *c, float width, float bar) {
    float left = 1.0f - (float)c->damage / 255.0f;
    if (left < 0.0f) left = 0.0f;

    // Green through amber to red, so a glance is enough.
    ImVec4 tint = { 0.35f + (1.0f - left) * 0.55f, 0.30f + left * 0.55f,
                    0.22f, 1.0f };
    if (c->wrecked) tint = (ImVec4){ 0.60f, 0.16f, 0.16f, 1.0f };

    ImGui_PushStyleColorImVec4(ImGuiCol_PlotHistogram, tint);
    ImGui_ProgressBar(left, (ImVec2){ width, bar }, "");
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

// **Where zero sits along the speed bar.** Left of it is reverse.
//
// A bar that fills from the left cannot say which way you are going, and going
// backwards is a thing this game asks of a driver often - reversing off a wall,
// out of a hole, back to a checkpoint that was driven past. So zero is a quarter
// of the way in and the bar grows both ways from it.
#define GS_HUD_SPEED_ZERO 0.25f

// **How tall the speed bar is against the condition bar.**
//
// Slimmer, because the two say different kinds of thing. Condition is the one
// that ends your race, and it is the one that should catch an eye that was not
// looking for it; speed is ambient, read continuously and never an alarm. A
// readout that shouts as loudly as the alarm beside it makes the alarm quieter.
//
// It also gives the panel back a couple of pixels in the states with the most
// rows in them, which is where a HUD fitted to a quarter of a small window has
// the least room to spare.
#define GS_HUD_SPEED_BAR 0.75f

// **How fast, and which way, as a bar rather than a number.** Asked for from
// play, and it is the same argument the damage bar already makes: a figure that
// changes every frame is read by nobody at speed, while a bar says "nearly flat
// out" or "going the wrong way" without being read at all.
//
// **One scale in both directions**, tiles per second per pixel, with zero
// offset rather than two scales meeting at a line. A bar whose halves mean
// different things is a bar you have to interpret, and the quarter given to
// reverse is exactly a third of the three quarters given to forward - so full
// forward is this machine's top speed and full reverse is a third of it, on the
// same ruler.
//
// Full forward is *this machine's* top speed, so the bar answers "how much of
// what I have am I using" rather than "how fast in the abstract" - the number a
// driver can act on, and one that means the same in a rover as in a sprint car.
// Both ends clamp: a car can beat its own top downhill, which is a real thing
// this game is about, and the last pixel meaning "more than the engine alone"
// is better than a bar that runs off its own end.
//
// The accent colour rather than the damage bar's green-to-red: colour there
// means how worried to be, and borrowing it here would say a fast car is a car
// in trouble. Reverse is drawn in the warning orange instead, because going
// backwards at speed is nearly always a thing to stop doing.
static void gs_hud_speed(const gs_car *c, float width, float bar) {
    const gs_vehicle_def *def = gs_vehicle(c->vehicle);
    const float top = def != nullptr ? gs_to_f(def->top) : 0.0f;

    // Along the way it is pointing, not how fast it is moving: a car sliding
    // backwards down a slope is going backwards, whatever its speed says.
    const gs_fix vlong = gs_fix_mul(c->vx, gs_cos(c->heading)) +
                         gs_fix_mul(c->vy, gs_sin(c->heading));
    const float now = gs_to_f(vlong);

    // Tiles a second per pixel, from the forward end - and the same ruler
    // backwards, which is what makes the reverse quarter mean anything.
    const float ahead = width * (1.0f - GS_HUD_SPEED_ZERO);
    float px = top > 0.0f ? (now / top) * ahead : 0.0f;
    const float back = width * GS_HUD_SPEED_ZERO;
    if (px > ahead) px = ahead;
    if (px < -back) px = -back;

    float r, g, b;
    gs_style_accent(&r, &g, &b);

    const ImVec2 at = ImGui_GetCursorScreenPos();
    ImDrawList *dl = ImGui_GetWindowDrawList();

    const float zero = at.x + back;
    const float y0 = at.y, y1 = at.y + bar;

    // The track it runs in, so an empty bar is still a bar.
    ImDrawList_AddRectFilled(dl, (ImVec2){ at.x, y0 },
                             (ImVec2){ at.x + width, y1 },
                             ImGui_GetColorU32ImVec4((ImVec4){ r * 0.25f,
                                                               g * 0.25f,
                                                               b * 0.25f, 0.55f }));

    if (px >= 0.0f) {
        ImDrawList_AddRectFilled(dl, (ImVec2){ zero, y0 },
                                 (ImVec2){ zero + px, y1 },
                                 ImGui_GetColorU32ImVec4((ImVec4){ r, g, b, 1.0f }));
    } else {
        ImDrawList_AddRectFilled(dl, (ImVec2){ zero + px, y0 },
                                 (ImVec2){ zero, y1 },
                                 ImGui_GetColorU32ImVec4((ImVec4){ 1.0f, 0.45f,
                                                                   0.20f, 1.0f }));
    }

    // The zero mark, drawn over the fill so it is still findable at a glance
    // when the bar is hard against it either way.
    ImDrawList_AddRectFilled(dl, (ImVec2){ zero - 1.0f, y0 },
                             (ImVec2){ zero + 1.0f, y1 },
                             ImGui_GetColorU32ImVec4((ImVec4){ 0.92f, 0.92f,
                                                               0.92f, 0.9f }));

    // The row the bar occupies, claimed the same way a progress bar claims one
    // so the panel's height arithmetic is unchanged by drawing it by hand.
    ImGui_Dummy((ImVec2){ width, bar });

    ImGui_PushStyleColorImVec4(ImGuiCol_Text, (ImVec4){ r, g, b, 0.85f });
    ImGui_TextUnformatted(now < -0.05f ? "speed  REVERSE" : "speed");
    ImGui_PopStyleColor();
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

// **The rows this panel is about to draw**, listed in the order it draws them.
// The size and the contents are one list rather than two that have to be kept
// in step - which they were not: every state was a little short, the plain one
// by eleven pixels, and the wreck message by a whole line. What is drawn below
// adds a row here, or the test that renders every state and asks what did not
// fit will say so.
typedef struct gs_hud_rows {
    float bigs, smalls, gaps;
    bool  finished, wrecked, waiting, online;

    // **What this car is carrying**, when it is carrying anything. A race with
    // the weapons off does not get the row at all, rather than getting a row
    // that says nothing - the panel is sized from the rows it has, and a row
    // that exists for every race and means something in one of them is a hole
    // in all the others.
    bool  carrying;

    // **A checkpoint driven past.** Its own row, because a driver who has
    // missed one is about to finish a lap that will not count and the game has
    // to say so before they cross the line rather than after.
    bool  missed;
} gs_hud_rows;

// **What a line of text will actually measure.** ImGui bakes a font at whole
// pixels and rounds a scaled size to get there, so a line asked for at 12.28
// comes back at 12 - and a panel sized from the number it asked for is a pixel
// per row too tall. Fifteen rows of that is fifteen pixels of nothing at the
// bottom of the box, which is precisely what the test that watches for a hole
// in this panel is for.
static float gs_hud_line(float base, float scale) {
    float h = SDL_roundf(base * scale);
    return h < 1.0f ? 1.0f : h;
}

// **Every size it is built from is an argument.** Not for flexibility: it is
// what lets the panel be fitted to a view too short for it. Halve the four and
// the answer roughly halves - roughly, because of the rounding above - so the
// fraction that fits is found by dividing and then stepping, rather than by the
// panel spilling over the player below.
static float gs_hud_height(const gs_hud_rows *r, float base, float zoom,
                           float gap, float pad, float bar) {
    const float body = gs_hud_line(base, zoom);
    const float big = gs_hud_line(base, GS_HUD_BIG * zoom);
    const float small = gs_hud_line(base, GS_HUD_SMALL * zoom);

    const float row_big = big + body + gap * 2.0f;
    const float row_small = small + body + gap * 2.0f;

    float h = pad * 2.0f
            + row_big * r->bigs
            + row_small * r->smalls
            // Two bars, each labelled: how fast, and what is left of the car.
            // The speed bar is the slimmer of the two on purpose - see
            // GS_HUD_SPEED_BAR.
            + bar * GS_HUD_SPEED_BAR + gap * 2.0f + body
            + bar + gap * 2.0f + body
            + gap * r->gaps;
    // **What it is carrying.** Its own height and no gap after it - unlike the
    // finished row below, which costs a gap more because it is the last thing
    // on the panel and ImGui's content ends at the last item rather than after
    // the spacing that would follow it.
    //
    // Measured rather than derived. `gs_hud_spare` reports what the panel has
    // left over and the test fails on a hole, so the arithmetic here is checked
    // against what actually got drawn in all twenty-four states - which is how
    // this row was caught costing a gap too much in exactly the three where
    // somebody is waiting.
    // **A row that ends the panel costs one gap more than a row in the middle**
    // - measured, not reasoned: the panel is nine pixels short whenever its
    // last row is one of these and exactly right when something follows it.
    // Both of the rows below were charged for one case and drawn in both.
    //
    // Carrying was charged as though something always followed it, and in the
    // commonest race of all - weapons on, nobody wrecked, nobody waiting - it
    // is the last thing on the panel. It fitted anyway because there was slack
    // above to eat the difference; adding the speed bar spent that slack and
    // the shortfall came out.
    // **And the gap under it.** Charged without one, on the reasoning that
    // ImGui's content ends at the last item rather than after the spacing that
    // would follow it - which left the panel nine pixels short in every race
    // with weapons in it. It fitted only because there was slack above to eat
    // the difference, and adding the speed bar spent that slack.
    if (r->carrying) h += row_small + gap;
    // The missed checkpoint, and the gap above it.
    if (r->missed) h += row_small + gap;
    // A finished car's time, and the gap above it.
    if (r->finished) h += row_small + gap;
    // The one or two keys a wrecked driver is offered. Counted here rather than
    // hoped for: a panel sized before its contents is how a button ends up half
    // outside the box it is in.
    if (r->wrecked) h += (body + gap) * (r->online ? 1.0f : 2.0f);
    // And the race waiting for somebody: what is happening, how long it has
    // been happening, and - when there is no wreck message already saying it -
    // the way out.
    if (r->waiting) {
        h += gap + (body + gap) * 2.0f;
        if (!r->wrecked) h += (body + gap) * (r->online ? 1.0f : 2.0f);
    }
    return h;
}

// The same, for a candidate fraction of full size: everything scales together,
// so a smaller HUD is the same HUD rather than the same text in a squashed box.
static float gs_hud_at(const gs_hud_rows *r, float base, const ImGuiStyle *st,
                       float zoom) {
    return gs_hud_height(r, base, zoom, st->ItemSpacing.y * zoom,
                         st->WindowPadding.y * zoom, GS_HUD_BAR * zoom);
}

// --- the minimap ------------------------------------------------------------
//
// **The whole track, seen from above, the way the original showed it.**
//
// The race is isometric and zoomed to where the car is, which is right for
// driving and useless for knowing where you are: at one tile to sixty-four
// pixels a player sees about ten tiles of a track sixty across, one gate arrow
// at a time and no road edge. Somebody read a gentle left-to-right sprint as
// two switchback turns from that view and had no way to find out otherwise.
// The line painted on the ground answers "which way now"; this answers "where
// am I on it", which is a different question and wants a different picture.
//
// Top down and not isometric, because a map is for reading and an isometric map
// is a picture of a map.

// The longest side of the map, in pixels, before the track's own shape is
// fitted into it.
#define GS_MAP_MAX  132.0f
#define GS_MAP_EDGE 6.0f          // inside the panel, so nothing touches the frame

// How many samples of the route curve one leg gets. Enough that a corner reads
// as a curve at this size rather than as two straight lines.
#define GS_MAP_STEPS 10

static ImU32 gs_map_rgba(float r, float g, float b, float a) {
    return (ImU32)((uint32_t)(a * 255.0f) << 24 | (uint32_t)(b * 255.0f) << 16 |
                   (uint32_t)(g * 255.0f) << 8 | (uint32_t)(r * 255.0f));
}

// The shape itself - route, finish line, gates - shared with the tracks
// screen's preview, which is the whole reason it is a function: two drawings
// of "what does this track look like" drift, one cannot.
int gs_hud_track_shape(const gs_track *t, ImDrawList *dl, float ox, float oy,
                       float scale) {
    // A track with no route on it has nothing to draw and no shape to say
    // where you are in - the construction set's blank field, before anybody
    // has put a gate down.
    if (t == nullptr || dl == nullptr) return 0;
    if (t->w == 0 || t->h == 0 || gs_track_route_legs(t) == 0) return 0;

    // The route, in the blue it is painted in on the ground - the same curve
    // from the same function, so the map and the track agree.
    const ImU32 ink = gs_map_rgba(0.30f, 0.65f, 0.95f, 0.95f);
    uint8_t legs = gs_track_route_legs(t);
    int drawn = 0;
    for (uint8_t leg = 0; leg < legs; leg++) {
        for (int k = 0; k < GS_MAP_STEPS; k++) {
            gs_fix ax, ay, bx, by;
            gs_track_route_point(t, leg,
                                 (gs_fix)((int64_t)k * GS_ONE / GS_MAP_STEPS),
                                 &ax, &ay);
            gs_track_route_point(t, leg,
                                 (gs_fix)((int64_t)(k + 1) * GS_ONE / GS_MAP_STEPS),
                                 &bx, &by);
            ImDrawList_AddLineEx(dl,
                (ImVec2){ ox + gs_to_f(ax) * scale, oy + gs_to_f(ay) * scale },
                (ImVec2){ ox + gs_to_f(bx) * scale, oy + gs_to_f(by) * scale },
                ink, 2.0f);
            drawn++;
        }
    }

    // The line you cross to finish, marked across the route rather than along
    // it, so a loop says where it begins.
    const gs_gate *fin = &t->gate[gs_track_finish_gate(t)];
    float fx = gs_to_f(gs_cos(fin->heading)), fy = gs_to_f(gs_sin(fin->heading));
    float hw = gs_to_f(fin->half_width);
    ImDrawList_AddLineEx(dl,
        (ImVec2){ ox + (gs_to_f(fin->x) - fy * hw) * scale,
                  oy + (gs_to_f(fin->y) + fx * hw) * scale },
        (ImVec2){ ox + (gs_to_f(fin->x) + fy * hw) * scale,
                  oy + (gs_to_f(fin->y) - fx * hw) * scale },
        gs_map_rgba(0.95f, 0.95f, 0.95f, 0.95f), 2.0f);

    // Every gate as a dot, small enough that ninety of them read as beads on
    // the route rather than as a second line.
    for (uint8_t i = 0; i < t->gate_count; i++) {
        const gs_gate *g = &t->gate[i];
        ImDrawList_AddCircleFilled(dl,
            (ImVec2){ ox + gs_to_f(g->x) * scale, oy + gs_to_f(g->y) * scale },
            1.5f, gs_map_rgba(0.75f, 0.85f, 0.95f, 0.7f), 0);
    }

    return drawn;
}

static void gs_hud_minimap(const gs_world *w, const gs_track *t, const gs_view *v) {
    // A track with no route on it has nothing to draw and no shape to say
    // where you are in - the construction set's blank field, before anybody has
    // put a gate down.
    if (t->w == 0 || t->h == 0 || gs_track_route_legs(t) == 0) return;

    float tw = (float)t->w, th = (float)t->h;
    float scale = GS_MAP_MAX / (tw > th ? tw : th);
    float bw = tw * scale, bh = th * scale;

    // **The corner the stats panel is not in.** Two panels in one corner is one
    // panel with the other one hidden behind it, and at four players each view
    // has its own of both.
    ImGui_SetNextWindowPos(
        (ImVec2){ (float)(v->rect.x + v->rect.w) - GS_HUD_PAD - bw - GS_MAP_EDGE * 2.0f,
                  (float)v->rect.y + GS_HUD_PAD },
        ImGuiCond_Always);
    ImGui_SetNextWindowSize((ImVec2){ bw + GS_MAP_EDGE * 2.0f, bh + GS_MAP_EDGE * 2.0f },
                            ImGuiCond_Always);
    ImGui_SetNextWindowBgAlpha(0.45f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    char id[32];
    SDL_snprintf(id, sizeof id, "##map%d", (int)v->car);
    if (ImGui_Begin(id, nullptr, flags)) {
        ImDrawList *dl = ImGui_GetWindowDrawList();
        ImVec2 at = ImGui_GetWindowPos();
        float ox = at.x + GS_MAP_EDGE, oy = at.y + GS_MAP_EDGE;

        // The shape - route, finish line, gate beads - from the one function
        // the tracks screen's preview also draws with.
        gs_hud_track_shape(t, dl, ox, oy, scale);

        // **The checkpoint this driver owes**, ringed on top of its bead.
        //
        // The route says which way round; it does not say what you have to go
        // *through*. A player who ran wide at a corner drove the rest of the
        // lap, crossed the chequer, and only found out at the end that none of
        // it counted - and asked for the checkpoints on the map, which is the
        // question answered before the mistake rather than after it. The ring
        // is white while the gate is ahead and the warning's orange once it
        // has been driven past, because then the lap depends on going back.
        const gs_car *me = v->car < w->car_count ? &w->car[v->car] : nullptr;
        if (me != nullptr && me->finish_tick == 0 &&
            me->next_gate < t->gate_count) {
            const gs_gate *g = &t->gate[me->next_gate];
            const ImU32 mark = v->missed
                                   ? gs_map_rgba(1.0f, 0.35f, 0.20f, 1.0f)
                                   : gs_map_rgba(1.0f, 1.0f, 1.0f, 1.0f);
            ImDrawList_AddCircleEx(dl,
                (ImVec2){ ox + gs_to_f(g->x) * scale,
                          oy + gs_to_f(g->y) * scale },
                4.0f, mark, 0, 2.0f);
        }

        // Everybody on it, in the colour they are driving, and this machine's
        // car ringed so it is findable at a glance rather than counted out.
        for (uint8_t i = 0; i < w->car_count; i++) {
            const gs_car *c = &w->car[i];
            if (!c->active) continue;

            // The colour this car is being drawn in, asked of the renderer
            // rather than of the car: paint is presentation, and the world
            // state is deliberately free of it.
            SDL_FColor paint = gs_render_paint_colour(gs_render_car_paint(i));
            ImVec2 p = { ox + gs_to_f(c->x) * scale, oy + gs_to_f(c->y) * scale };

            if (i == v->car) {
                ImDrawList_AddCircleFilled(dl, p, 4.5f,
                                           gs_map_rgba(1.0f, 1.0f, 1.0f, 0.9f), 0);
            }
            ImDrawList_AddCircleFilled(
                dl, p, 3.0f, gs_map_rgba(paint.r, paint.g, paint.b, 1.0f), 0);
        }
    }
    ImGui_End();
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
    const bool derby = w->mode == (uint8_t)GS_MODE_DESTRUCTION;
    const bool counting = gs_world_countdown(w) > 0;

    gs_hud_rows rows = {
        // **A derby is not a race and its HUD is not the same size.** Where you
        // are in the order, which lap it is, how long this one is taking and
        // the best one so far are four rows about getting round a track, and
        // "last one driving" is not about that. What it draws instead is one
        // row - how many are left - so it is one row tall plus whatever the
        // countdown is doing.
        .bigs = derby ? 1.0f : 2.0f,          // still driving | position, lap
        .smalls = derby ? (counting ? 1.0f : 0.0f)   // get ready
                        : 2.0f,                       // this lap, best
        .finished = c->finish_tick != 0,
        .carrying = gs_car_selected(c) != GS_HAZ_NONE,
        .missed = v->missed && c->finish_tick == 0,
        .wrecked = c->wrecked,
        .waiting = waited > 0.5f,
        .online = online,
    };
    // A gap before each small row and one before the bar. An ImGui_Spacing() is
    // an item of no height that still costs a gap, and forgetting them is the
    // whole of the twenty-nine pixels this panel was short in every state it
    // had. In a race there is a gap between the two big rows as well.
    rows.gaps = rows.smalls + 1.0f + (derby ? 0.0f : 1.0f);

    // **And it has to fit the view it belongs to, which is not the window.**
    //
    // Every state of this panel was measured, and always in one view filling
    // the whole screen. Four players do not get that: the window splits four
    // ways and each view is a quarter of it. The HUD was sized from its rows
    // and nothing else, so at four players it was 331 pixels tall in a view 358
    // tall - and in six of its twelve states it was taller than that, drawn
    // over the player below and reading them somebody else's lap time. **At the
    // size the game opens at**, before anybody drags anything.
    //
    // ImGui clamps a window to the viewport, which is the whole screen. It has
    // never heard of a view.
    //
    // So the panel is drawn at whatever fraction of itself fits. Every size it
    // is built from scales together - the text, the gaps, the padding, the bar
    // and the width - so a quarter-screen HUD is the same HUD smaller rather
    // than the same text in a squashed box. There is no floor: the rule is that
    // it stays inside its view, and a legibility floor would be a rule that
    // holds until the window gets small enough to break it.
    const float base = ImGui_GetTextLineHeight();
    const ImGuiStyle *st = ImGui_GetStyle();

    const float room = (float)v->rect.h - GS_HUD_PAD * 2.0f;
    const float full = gs_hud_at(&rows, base, st, 1.0f);

    gs_hud_zoom = 1.0f;
    if (full > room && full > 0.0f) {
        gs_hud_zoom = room / full;
        // A hundredth at a time, because the rounding above means the fraction
        // that fits is near the one the division gives and not always at it.
        // Bounded, and it stops the moment it fits.
        //
        // **The step is the size of the hole it can leave.** It stops at the
        // first fraction that fits, so whatever it overshot by is empty panel
        // at the bottom - and a step of zoom is worth more pixels the taller
        // the panel, so the worst of it lands on the state with the most rows.
        // Five times finer costs a few more evaluations of a dozen multiplies,
        // once a frame.
        for (int i = 0; i < 500 && gs_hud_zoom > 0.05f &&
                        gs_hud_at(&rows, base, st, gs_hud_zoom) > room; i++) {
            gs_hud_zoom -= 0.002f;
        }
    }

    float width = GS_HUD_W * gs_hud_zoom;
    const float across = (float)v->rect.w - GS_HUD_PAD * 2.0f;
    if (width > across) width = across;

    const float height = gs_hud_at(&rows, base, st, gs_hud_zoom);

    // Pushed before Begin, because a window's padding is read as it opens.
    ImGui_PushStyleVarImVec2(ImGuiStyleVar_WindowPadding,
                             (ImVec2){ st->WindowPadding.x * gs_hud_zoom,
                                       st->WindowPadding.y * gs_hud_zoom });
    ImGui_PushStyleVarImVec2(ImGuiStyleVar_ItemSpacing,
                             (ImVec2){ st->ItemSpacing.x * gs_hud_zoom,
                                       st->ItemSpacing.y * gs_hud_zoom });

    ImGui_SetNextWindowSize((ImVec2){ width, height }, ImGuiCond_Always);

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

        // **A derby is not a race and its HUD should not pretend to be one.**
        //
        // Four of these five rows are about getting round a track: where you
        // are in the order, which lap it is, how long this one is taking, and
        // the best one so far. In "last one driving" none of that decides
        // anything - a car three corners ahead and a car sitting still are
        // equal until one of them is wrecked - and the one question the mode
        // does ask, how many are left, was not on the screen at all. It read
        // "position 1/4" with two of the four already wrecked.
        // --- First, and biggest, because it is the question. Which question it
        //     is depends on what is being played.
        if (derby) {
            SDL_snprintf(text, sizeof text, "%u", gs_world_driving(w, nullptr));
            gs_hud_stat("still driving", text, GS_HUD_BIG);
        } else {
            uint8_t place = gs_world_place(w, t, v->car);
            uint8_t racing = 0;
            for (uint8_t i = 0; i < w->car_count; i++) {
                if (w->car[i].active) racing++;
            }
            SDL_snprintf(text, sizeof text, "%u/%u", place, racing);
            gs_hud_stat("position", text, GS_HUD_BIG);
        }

        // --- Lap. The target counts from the world rather than the setup, so a
        // race whose length was changed shows the length being raced.
        if (!derby) ImGui_Spacing();
        // **Asked of the route rather than read off the car.** On a loop the
        // first crossing of the line is the run up to it and not a lap anybody
        // drove, so `laps` is one ahead of what a driver would say; and a path
        // has no laps at all, only an arrival. See gs_car_laps_done.
        if (!derby) {
            uint16_t needed = gs_world_laps_needed(w, t);
            uint16_t done = gs_car_laps_done(t, c);
            if (needed > 0) {
                uint16_t on = (uint16_t)(done + 1);
                if (on > needed) on = needed;
                SDL_snprintf(text, sizeof text, "%u/%u", on, needed);
            } else {
                SDL_snprintf(text, sizeof text, "%u", (uint16_t)(done + 1));
            }
            gs_hud_stat("lap", text, GS_HUD_BIG);
        }

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
        if (counting || !derby) ImGui_Spacing();
        if (counting) {
            const uint32_t left = gs_world_countdown(w);
            SDL_snprintf(text, sizeof text, "%u",
                         (unsigned)((left + (uint32_t)GS_TICK_HZ - 1u) /
                                    (uint32_t)GS_TICK_HZ));
            gs_hud_stat("get ready", text, GS_HUD_SMALL);
        } else if (derby) {
            // Nothing: a lap clock in a derby is a clock counting nothing.
        } else if (c->wrecked) {
            gs_hud_stat("this lap", "-", GS_HUD_SMALL);
        } else {
            uint32_t running = tick > c->lap_start ? tick - c->lap_start : 0;
            gs_time_text(text, sizeof text, running);
            gs_hud_stat("this lap", text, GS_HUD_SMALL);
        }

        // --- And the one to beat.
        if (!derby) {
            ImGui_Spacing();
            gs_time_text(text, sizeof text, c->best_lap);
            gs_hud_stat("best", text, GS_HUD_SMALL);
        }

        // --- How fast it is going, and then what is left of it.
        ImGui_Spacing();
        gs_hud_speed(c, ImGui_GetContentRegionAvail().x,
                     GS_HUD_BAR * GS_HUD_SPEED_BAR * gs_hud_zoom);

        ImGui_Spacing();
        // **As wide as the room there is**, not as wide as the window less a
        // padding somebody wrote down once. The HUD is drawn in the menu's
        // style, whose window padding is twenty-two a side rather than the
        // eight assumed here, so the bar was twenty-eight pixels wider than the
        // panel it sits in and ran off the edge of it.
        gs_hud_damage(c, ImGui_GetContentRegionAvail().x,
                      GS_HUD_BAR * gs_hud_zoom);

        // **What a tap would leave, and how many are left.** Without this the
        // hold that changes the selection changes something invisible, which is
        // not a control - and the setup screen, which is the only other place
        // it is said, is gone by the time anybody is driving.
        gs_hud_carried[0] = '\0';
        if (rows.carrying) {
            ImGui_Spacing();
            const gs_hazard_kind sel = gs_car_selected(c);
            SDL_snprintf(text, sizeof text, "%s %u",
                         gs_hazard_name(sel), gs_car_ammo(c, sel));
            gs_hud_stat("carrying", text, GS_HUD_SMALL);
            SDL_snprintf(gs_hud_carried, sizeof gs_hud_carried, "%s", text);
        }

        // **A checkpoint driven past, said before the flag rather than after.**
        // The race only ever tests the gate this car is owed, so from here the
        // finish will do nothing - and a player who ran wide at a corner and
        // was told nothing drove the whole rest of the lap and crossed the
        // chequer for it. The arrow on the ground points back at the one owed.
        if (v->missed && c->finish_tick == 0) {
            ImGui_Spacing();
            // "checkpoint missed" does not fit the panel, whose width is set by
            // the shortest labels in the game; it was drawn as "checkpoint
            // misse". The word that matters is the instruction.
            gs_hud_stat("checkpoint", "GO BACK", GS_HUD_SMALL);
        }

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

        // **And what was left over.** The room under the last thing drawn,
        // which is the other half of the same question: a panel sized for rows
        // it is not drawing has a hole in it, and asking what fell off the
        // bottom cannot see a hole.
        gs_hud_room = ImGui_GetContentRegionAvail().y;
        if (gs_hud_room < 0.0f) gs_hud_room = 0.0f;
    }
    ImGui_End();
    ImGui_PopStyleVar();
    ImGui_PopStyleVar();

    // **And where that is on the track**, in the corner the stats are not in.
    // Drawn after them so that if the two ever meet - a view narrow enough for
    // both to want the same pixels - the map is the one on top, and a map with
    // a corner over the lap counter is a great deal less confusing than a lap
    // counter over the map.
    gs_hud_minimap(w, t, v);
}

const char *gs_hud_carrying(void) { return gs_hud_carried; }

float gs_hud_overflow(void) { return gs_hud_hidden; }
float gs_hud_spare(void) { return gs_hud_room; }

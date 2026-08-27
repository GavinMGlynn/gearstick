// The implementation of gs_ui_probe.h - see there for what this is for.
//
// This is the one first-party file in the tree that has to be C++: the hooks
// Dear ImGui calls take ImGui's own C++ types, so the signatures are not
// expressible in C. Everything it offers outwards is C.

#include "gs_ui_probe.h"

// ImGui's own headers are held to ImGui's standards, not ours. They carry
// inline code full of deliberate narrowing into bitfields, which is fine in a
// library built under its own settings and fatal under -Wconversion -Werror.
// The set stays on for everything below the pop - suppressing warnings for a
// whole file to quieten somebody else's header is how our own slips get in.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include "imgui.h"
#include "imgui_internal.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <stdarg.h>
#include <string.h>

namespace {

gs_ui_item *g_into     = nullptr;
int         g_capacity = 0;
int         g_count    = 0;

// The entry for an id, if this frame has one and there was room for it.
gs_ui_item *gs_slot(ImGuiID id)
{
    if (g_into == nullptr) return nullptr;
    const int held = g_count < g_capacity ? g_count : g_capacity;
    for (int i = 0; i < held; i++) {
        if (g_into[i].id == id) return &g_into[i];
    }
    return nullptr;
}

// ImGui labels carry their id after a "##", which is not part of the name a
// person would use. "##login" has no visible part at all, and for those the
// whole label is kept - a name nobody sees is still better than no name.
void gs_name(gs_ui_item *it, const char *label)
{
    const char *hash = strstr(label, "##");
    size_t n = (hash != nullptr) ? (size_t)(hash - label) : strlen(label);
    if (n == 0) n = strlen(label);
    if (n > GS_UI_LABEL - 1) n = GS_UI_LABEL - 1;
    memcpy(it->label, label, n);
    it->label[n] = '\0';
}

} // namespace

void gs_ui_probe_start(gs_ui_item *into, int capacity)
{
    g_into     = into;
    g_capacity = capacity > 0 ? capacity : 0;
    g_count    = 0;

    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx != nullptr) ctx->TestEngineHookItems = true;
}

void gs_ui_probe_stop(void)
{
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx != nullptr) ctx->TestEngineHookItems = false;

    g_into     = nullptr;
    g_capacity = 0;
    g_count    = 0;
}

void gs_ui_probe_frame(void) { g_count = 0; }

int gs_ui_probe_count(void) { return g_count; }

void gs_ui_probe_press(uint32_t id)
{
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::ActivateItemByID((ImGuiID)id);
    }
}

void gs_ui_probe_type(const char *text)
{
    if (ImGui::GetCurrentContext() == nullptr || text == nullptr) return;
    ImGui::GetIO().AddInputCharactersUTF8(text);
}

bool gs_ui_probe_focus_window(const char *name)
{
    if (ImGui::GetCurrentContext() == nullptr || name == nullptr) return false;
    ImGuiWindow *w = ImGui::FindWindowByName(name);
    if (w == nullptr) return false;
    ImGui::FocusWindow(w);
    ImGui::SetNavWindow(w);
    return true;
}

bool gs_ui_probe_wheel(const char *window, float ticks)
{
    if (ImGui::GetCurrentContext() == nullptr || window == nullptr) return false;
    ImGuiWindow *w = ImGui::FindWindowByName(window);
    if (w == nullptr) return false;

    // Over the middle of it, because ImGui gives the wheel to the window under
    // the pointer and to nothing else - a wheel event with the mouse parked at
    // the origin scrolls whatever happens to be in the corner.
    ImGuiIO &io = ImGui::GetIO();
    io.AddMousePosEvent(w->Pos.x + w->Size.x * 0.5f,
                        w->Pos.y + w->Size.y * 0.5f);
    io.AddMouseWheelEvent(0.0f, ticks);
    return true;
}

bool gs_ui_probe_scroll_at(const char *window, float *now, float *max)
{
    if (ImGui::GetCurrentContext() == nullptr || window == nullptr) return false;
    ImGuiWindow *w = ImGui::FindWindowByName(window);
    if (w == nullptr) return false;
    if (now != nullptr) *now = w->Scroll.y;
    if (max != nullptr) *max = w->ScrollMax.y;
    return true;
}

bool gs_ui_probe_scroll_span(const char *window, float *x, float *y,
                             float *max_x, float *max_y)
{
    if (ImGui::GetCurrentContext() == nullptr || window == nullptr) return false;
    ImGuiWindow *w = ImGui::FindWindowByName(window);
    if (w == nullptr) return false;
    if (x != nullptr)     *x     = w->Scroll.x;
    if (y != nullptr)     *y     = w->Scroll.y;
    if (max_x != nullptr) *max_x = w->ScrollMax.x;
    if (max_y != nullptr) *max_y = w->ScrollMax.y;
    return true;
}

bool gs_ui_probe_scrollbars(const char *window, bool *x, bool *y)
{
    if (ImGui::GetCurrentContext() == nullptr || window == nullptr) return false;
    ImGuiWindow *w = ImGui::FindWindowByName(window);
    if (w == nullptr) return false;
    if (x != nullptr) *x = w->ScrollbarX;
    if (y != nullptr) *y = w->ScrollbarY;
    return true;
}

bool gs_ui_probe_scroll_to(const char *window, float x, float y)
{
    if (ImGui::GetCurrentContext() == nullptr || window == nullptr) return false;
    ImGuiWindow *w = ImGui::FindWindowByName(window);
    if (w == nullptr) return false;
    ImGui::SetScrollX(w, x);
    ImGui::SetScrollY(w, y);
    return true;
}

bool gs_ui_probe_window_box(const char *name, float *x, float *y,
                            float *w, float *h)
{
    if (ImGui::GetCurrentContext() == nullptr || name == nullptr) return false;
    ImGuiWindow *win = ImGui::FindWindowByName(name);
    if (win == nullptr) return false;
    if (x != nullptr) *x = win->Pos.x;
    if (y != nullptr) *y = win->Pos.y;
    if (w != nullptr) *w = win->Size.x;
    if (h != nullptr) *h = win->Size.y;
    return true;
}

bool gs_ui_probe_unfold(const char *name)
{
    if (ImGui::GetCurrentContext() == nullptr || name == nullptr) return false;
    ImGuiWindow *win = ImGui::FindWindowByName(name);
    if (win == nullptr) return false;
    if (!win->Collapsed) return false;
    ImGui::SetWindowCollapsed(win, false);
    return true;
}

uint32_t gs_ui_probe_focused(void)
{
    const ImGuiContext *ctx = ImGui::GetCurrentContext();
    return (ctx != nullptr) ? (uint32_t)ctx->NavId : 0u;
}

void gs_ui_probe_settle(void)
{
    if (ImGui::GetCurrentContext() == nullptr) return;
    ImGui::ClosePopupsExceptModals();
    ImGui::ClearActiveID();
}

// --- The hooks themselves. Declared by imgui_internal.h, called by imgui.cpp
//     when TestEngineHookItems is on, and by nothing at all when it is not.

void ImGuiTestEngineHook_ItemAdd(ImGuiContext *, ImGuiID id, const ImRect &bb,
                                 const ImGuiLastItemData *item_data)
{
    if (gs_slot(id) != nullptr) return;      // already counted this frame

    const int at = g_count;
    g_count++;
    if (g_into == nullptr || at >= g_capacity) return;

    const ImGuiItemFlags flags = (item_data != nullptr) ? item_data->ItemFlags
                                                        : (ImGuiItemFlags)0;
    gs_ui_item *it = &g_into[at];
    it->id        = id;
    it->label[0]  = '\0';
    it->window[0] = '\0';

    const ImGuiContext *ctx = ImGui::GetCurrentContext();
    if (ctx != nullptr && ctx->CurrentWindow != nullptr) {
        const char *w = ctx->CurrentWindow->Name;
        size_t n = strlen(w);
        if (n > GS_UI_WINDOW - 1) n = GS_UI_WINDOW - 1;
        memcpy(it->window, w, n);
        it->window[n] = '\0';
    }
    it->disabled  = (flags & ImGuiItemFlags_Disabled) != 0;
    it->reachable = (flags & ImGuiItemFlags_NoNav) == 0;
    it->typable   = false;
    it->visible   = true;
    it->whole     = true;
    it->heading   = false;
    it->x0        = bb.Min.x;
    it->y0        = bb.Min.y;
    it->x1        = bb.Max.x;
    it->y1        = bb.Max.y;

    // The hook runs before ImGui's own clipping test, which is what makes a
    // scrolled-away table row look like a control. **ImGui's rule, copied
    // rather than approximated**: an item outside the clip rectangle is
    // dropped, *unless* it is the one being interacted with or the one the
    // keyboard is on - those stay live so a control does not die under the
    // hand that is using it. An approximation here would report a focused item
    // as unpressable and lose a real control from the count.
    if (ctx != nullptr && ctx->CurrentWindow != nullptr) {
        const ImRect &clip = ctx->CurrentWindow->ClipRect;
        if (!bb.Overlaps(clip)) {
            it->visible = id == ctx->ActiveId || id == ctx->ActiveIdPreviousFrame ||
                          id == ctx->NavId    || id == ctx->NavActivateId;
        }
        it->whole = bb.Min.x >= clip.Min.x && bb.Max.x <= clip.Max.x &&
                    bb.Min.y >= clip.Min.y && bb.Max.y <= clip.Max.y;
    }

    // Inside the row a table opened with ImGuiTableRowFlags_Headers, which is
    // what TableHeadersRow() does and nothing else does.
    if (ctx != nullptr && ctx->CurrentTable != nullptr) {
        it->heading =
            (ctx->CurrentTable->RowFlags & ImGuiTableRowFlags_Headers) != 0;
    }
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext *, ImGuiID id, const char *label,
                                  ImGuiItemStatusFlags flags)
{
    gs_ui_item *it = gs_slot(id);
    if (it == nullptr) return;
    if (label != nullptr) gs_name(it, label);

    // ImGui says which items take text of their own accord - a box, or a slider
    // that can be typed into rather than dragged.
    it->typable = (flags & ImGuiItemStatusFlags_Inputable) != 0;
}

void ImGuiTestEngineHook_Log(ImGuiContext *, const char *, ...) {}

const char *ImGuiTestEngine_FindItemDebugLabel(ImGuiContext *, ImGuiID id)
{
    const gs_ui_item *it = gs_slot(id);
    return (it != nullptr && it->label[0] != '\0') ? it->label : nullptr;
}

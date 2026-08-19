#include "ui/gs_style.h"

#include "dcimgui.h"

// **This is dougbinks' "dark clear", ported.**
//
// See ext/imgui_styles, which is pinned as a submodule because it was read
// while writing this. The idea in it is better than any list of colours: there
// is *one* palette, written light, and "dark" is a transformation of it rather
// than a second palette to keep in step with the first. Invert the value of
// every colour that has almost no saturation - the greys, the backgrounds, the
// text - and leave the ones that do alone, so the blue accent stays exactly the
// blue it was while everything around it turns inside out.
//
// "Clear" is the other half: anything already partly transparent gets more so,
// which is what puts the track underneath the menu instead of behind it.
//
// The colours here are theirs. The spacing, padding and sizes are ours, because
// that screenshot is a debug overlay on a voxel editor and this is a title
// screen - the look travels, the density does not.

#define GS_CLEAR_ALPHA 0.72f

void gs_style_accent(float *r, float *g, float *b) {
    // The blue that survives the inversion, and therefore the one thing on
    // screen that means "press this".
    *r = 0.26f; *g = 0.59f; *b = 0.98f;
}

// The light base, exactly as the original has it.
static void gs_base_palette(ImGuiStyle *s) {
    ImVec4 *c = s->Colors;
    c[ImGuiCol_Text]                 = (ImVec4){ 0.00f, 0.00f, 0.00f, 1.00f };
    c[ImGuiCol_TextDisabled]         = (ImVec4){ 0.60f, 0.60f, 0.60f, 1.00f };
    c[ImGuiCol_WindowBg]             = (ImVec4){ 0.94f, 0.94f, 0.94f, 0.94f };
    c[ImGuiCol_ChildBg]              = (ImVec4){ 0.00f, 0.00f, 0.00f, 0.00f };
    c[ImGuiCol_PopupBg]              = (ImVec4){ 1.00f, 1.00f, 1.00f, 0.94f };
    c[ImGuiCol_Border]               = (ImVec4){ 0.00f, 0.00f, 0.00f, 0.39f };
    c[ImGuiCol_BorderShadow]         = (ImVec4){ 1.00f, 1.00f, 1.00f, 0.10f };
    c[ImGuiCol_FrameBg]              = (ImVec4){ 1.00f, 1.00f, 1.00f, 0.94f };
    c[ImGuiCol_FrameBgHovered]       = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.40f };
    c[ImGuiCol_FrameBgActive]        = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.67f };
    c[ImGuiCol_TitleBg]              = (ImVec4){ 0.96f, 0.96f, 0.96f, 1.00f };
    c[ImGuiCol_TitleBgCollapsed]     = (ImVec4){ 1.00f, 1.00f, 1.00f, 0.51f };
    c[ImGuiCol_TitleBgActive]        = (ImVec4){ 0.82f, 0.82f, 0.82f, 1.00f };
    c[ImGuiCol_MenuBarBg]            = (ImVec4){ 0.86f, 0.86f, 0.86f, 1.00f };
    c[ImGuiCol_ScrollbarBg]          = (ImVec4){ 0.98f, 0.98f, 0.98f, 0.53f };
    c[ImGuiCol_ScrollbarGrab]        = (ImVec4){ 0.69f, 0.69f, 0.69f, 1.00f };
    c[ImGuiCol_ScrollbarGrabHovered] = (ImVec4){ 0.59f, 0.59f, 0.59f, 1.00f };
    c[ImGuiCol_ScrollbarGrabActive]  = (ImVec4){ 0.49f, 0.49f, 0.49f, 1.00f };
    c[ImGuiCol_CheckMark]            = (ImVec4){ 0.26f, 0.59f, 0.98f, 1.00f };
    c[ImGuiCol_SliderGrab]           = (ImVec4){ 0.24f, 0.52f, 0.88f, 1.00f };
    c[ImGuiCol_SliderGrabActive]     = (ImVec4){ 0.26f, 0.59f, 0.98f, 1.00f };
    c[ImGuiCol_Button]               = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.40f };
    c[ImGuiCol_ButtonHovered]        = (ImVec4){ 0.26f, 0.59f, 0.98f, 1.00f };
    c[ImGuiCol_ButtonActive]         = (ImVec4){ 0.06f, 0.53f, 0.98f, 1.00f };
    c[ImGuiCol_Header]               = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.31f };
    c[ImGuiCol_HeaderHovered]        = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.80f };
    c[ImGuiCol_HeaderActive]         = (ImVec4){ 0.26f, 0.59f, 0.98f, 1.00f };
    c[ImGuiCol_ResizeGrip]           = (ImVec4){ 1.00f, 1.00f, 1.00f, 0.50f };
    c[ImGuiCol_ResizeGripHovered]    = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.67f };
    c[ImGuiCol_ResizeGripActive]     = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.95f };
    c[ImGuiCol_PlotLines]            = (ImVec4){ 0.39f, 0.39f, 0.39f, 1.00f };
    c[ImGuiCol_PlotLinesHovered]     = (ImVec4){ 1.00f, 0.43f, 0.35f, 1.00f };
    c[ImGuiCol_PlotHistogram]        = (ImVec4){ 0.90f, 0.70f, 0.00f, 1.00f };
    c[ImGuiCol_PlotHistogramHovered] = (ImVec4){ 1.00f, 0.60f, 0.00f, 1.00f };
    c[ImGuiCol_TextSelectedBg]       = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.35f };
    c[ImGuiCol_ModalWindowDimBg]     = (ImVec4){ 0.20f, 0.20f, 0.20f, 0.35f };

    // Things the original predates. Given the same treatment by hand: the
    // separators follow the old column colours, and the table chrome follows
    // the header and border it sits between.
    c[ImGuiCol_Separator]            = (ImVec4){ 0.39f, 0.39f, 0.39f, 1.00f };
    c[ImGuiCol_SeparatorHovered]     = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.78f };
    c[ImGuiCol_SeparatorActive]      = (ImVec4){ 0.26f, 0.59f, 0.98f, 1.00f };
    c[ImGuiCol_Tab]                  = (ImVec4){ 0.86f, 0.86f, 0.86f, 1.00f };
    c[ImGuiCol_TabHovered]           = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.80f };
    c[ImGuiCol_TabSelected]          = (ImVec4){ 0.26f, 0.59f, 0.98f, 0.60f };
    c[ImGuiCol_TableHeaderBg]        = (ImVec4){ 0.86f, 0.86f, 0.86f, 1.00f };
    c[ImGuiCol_TableBorderStrong]    = (ImVec4){ 0.39f, 0.39f, 0.39f, 1.00f };
    c[ImGuiCol_TableBorderLight]     = (ImVec4){ 0.59f, 0.59f, 0.59f, 1.00f };
    c[ImGuiCol_TableRowBg]           = (ImVec4){ 0.00f, 0.00f, 0.00f, 0.00f };
    c[ImGuiCol_TableRowBgAlt]        = (ImVec4){ 0.30f, 0.30f, 0.30f, 0.09f };
    c[ImGuiCol_NavCursor]            = (ImVec4){ 0.26f, 0.59f, 0.98f, 1.00f };
}

// Dark, and clear. The transformation is the whole trick and is quoted from the
// original rather than reinvented: turn the greys inside out, leave the colours
// where they are, and let whatever was already translucent become more so.
static void gs_dark_and_clear(ImGuiStyle *s, float alpha) {
    for (int i = 0; i < ImGuiCol_COUNT; i++) {
        ImVec4 *col = &s->Colors[i];

        float h, sat, v;
        ImGui_ColorConvertRGBtoHSV(col->x, col->y, col->z, &h, &sat, &v);
        if (sat < 0.1f) v = 1.0f - v;
        ImGui_ColorConvertHSVtoRGB(h, sat, v, &col->x, &col->y, &col->z);

        if (col->w < 1.0f) col->w *= alpha;
    }
}

static void gs_shared_shape(ImGuiStyle *s) {
    s->FrameRounding     = 3.0f;
    s->WindowRounding    = 4.0f;
    s->ChildRounding     = 3.0f;
    s->PopupRounding     = 3.0f;
    s->GrabRounding      = 3.0f;
    s->ScrollbarRounding = 6.0f;
    s->TabRounding       = 3.0f;

    s->WindowBorderSize  = 1.0f;
    s->FrameBorderSize   = 0.0f;
    s->PopupBorderSize   = 1.0f;

    s->WindowTitleAlign  = (ImVec2){ 0.5f, 0.5f };
    s->ButtonTextAlign   = (ImVec2){ 0.5f, 0.5f };
}

void gs_style_menu(void) {
    ImGuiStyle *s = ImGui_GetStyle();

    gs_base_palette(s);
    gs_dark_and_clear(s, GS_CLEAR_ALPHA);
    gs_shared_shape(s);

    // Room to breathe, which the source of these colours has none of and does
    // not want: it is a debug overlay on a voxel editor, and a title screen is
    // not. Fewer things, further apart, easier to hit.
    s->WindowPadding        = (ImVec2){ 22.0f, 18.0f };
    s->FramePadding         = (ImVec2){ 12.0f, 7.0f };
    s->ItemSpacing          = (ImVec2){ 10.0f, 9.0f };
    s->ItemInnerSpacing     = (ImVec2){ 8.0f, 6.0f };
    s->CellPadding          = (ImVec2){ 8.0f, 5.0f };
    s->IndentSpacing        = 20.0f;
    s->ScrollbarSize        = 13.0f;
    s->GrabMinSize          = 12.0f;
    s->SeparatorTextAlign   = (ImVec2){ 0.0f, 0.5f };
    s->SeparatorTextPadding = (ImVec2){ 18.0f, 6.0f };
}

void gs_style_editor(void) {
    ImGuiStyle *s = ImGui_GetStyle();

    gs_base_palette(s);

    // Less see-through for the tool. A brush palette is read while the thing
    // underneath it is being changed, and a translucent panel over moving
    // terrain is a panel you have to squint at.
    gs_dark_and_clear(s, 0.94f);
    gs_shared_shape(s);

    // And a tool's density, which is what a construction set actually wants:
    // everything reachable at once, far more than room to breathe.
    s->WindowPadding    = (ImVec2){ 10.0f, 8.0f };
    s->FramePadding     = (ImVec2){ 6.0f, 3.0f };
    s->ItemSpacing      = (ImVec2){ 7.0f, 4.0f };
    s->ItemInnerSpacing = (ImVec2){ 5.0f, 4.0f };
    s->WindowRounding   = 3.0f;
    s->FrameRounding    = 2.0f;
    s->GrabMinSize      = 10.0f;
    s->SeparatorTextPadding = (ImVec2){ 12.0f, 3.0f };
}

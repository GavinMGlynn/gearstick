// gs_style.h - what the front end looks like.
//
// Dear ImGui's default look is a debug tool's, which is exactly right for the
// construction set and exactly wrong for a title screen. The editor keeps it -
// dense, small, everything on screen at once, which is what a tool wants. The
// front end gets this instead: bigger, quieter, fewer things fighting for
// attention, and one accent colour doing the pointing.
//
// The layout numbers - rounding, padding, spacing - follow the shape of the
// well-worn community themes rather than being invented here. The colours do
// not: they are taken from the game's own palette, so a menu sits on top of the
// terrain instead of floating over it looking like a different program.
#ifndef GS_STYLE_H
#define GS_STYLE_H

#include "core/gs_common.h"

// Apply the front-end look. Call once, after the ImGui context exists.
void gs_style_menu(void);

// Back to something tool-shaped, for the construction set.
void gs_style_editor(void);

// The accent, for the odd thing that has to be drawn by hand.
void gs_style_accent(float *r, float *g, float *b);

#endif // GS_STYLE_H

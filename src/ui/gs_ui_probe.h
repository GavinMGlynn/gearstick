#ifndef GS_UI_PROBE_H
#define GS_UI_PROBE_H

// **Every control the menu drew, whether or not a key can reach it.**
//
// Walking a front end by pressing Tab tells you about the controls Tab reaches,
// in the order it reaches them, and that is two untruths at once. A control
// disabled in whatever state the walk happens to be in is stepped over in
// silence, so it never appears at all; and "the fifth thing on the setup
// screen" stops meaning anything the moment somebody inserts a fourth. Neither
// is something to claim coverage from.
//
// Dear ImGui already reports every item it adds, to whoever asks - the hooks
// its own test engine uses. This is those hooks and nothing else: no test
// engine, no new submodule, about a hundred lines. What comes back is every
// control on a screen with its name, whether it was drawn dead, and whether the
// keyboard could have landed on it. A control can then be pressed *by name*,
// which is also what makes walking everything affordable - reaching the nth
// control costs one frame rather than n.

#include <stdbool.h>
#include <stdint.h>

#define GS_UI_LABEL  48
#define GS_UI_WINDOW 32

typedef struct gs_ui_item {
    // **The identity is the id, not the position and not the label.** It is
    // ImGui's own hash of how the control was made, so it survives a redraw and
    // survives somebody inserting a control above it - which is the whole
    // reason a map keyed on it stays true.
    uint32_t id;

    // What a person calls it. ImGui reports this for the widgets a person
    // presses - buttons, sliders, boxes, rows - and **not** for the structural
    // items it adds around them: a child region, a table cell, a group. Those
    // come back with an empty label and are still real items with real ids, so
    // the window is carried too and nothing is ever nameless.
    char     label[GS_UI_LABEL];
    char     window[GS_UI_WINDOW];

    bool     disabled;            // drawn, but refusing to be pressed
    bool     reachable;           // the keyboard can land on it
    bool     typable;             // a box that takes text rather than a press
} gs_ui_item;

#ifdef __cplusplus
extern "C" {
#endif

// Collect what each frame draws into `into`, until told to stop.
void gs_ui_probe_start(gs_ui_item *into, int capacity);
void gs_ui_probe_stop(void);

// Forget the frame just gone. Called before drawing the frame to be measured.
void gs_ui_probe_frame(void);

// How many controls that frame drew. **This can exceed the capacity given.**
// The count is the truth about the screen and the array is only what there was
// room for, and a walk that means to assert coverage has to know when the two
// disagree rather than quietly measuring the smaller one.
int gs_ui_probe_count(void);

// Press a control by id on the next frame - wherever it sits in the order, and
// whether or not a keyboard could have got to it.
void gs_ui_probe_press(uint32_t id);

// **Type at whatever is taking text.** A password is not something a walk can
// press its way past, and a front end whose door needs one is a front end a
// press-only walk gets carried through rather than opens.
void gs_ui_probe_type(const char *text);

// **Which control the keyboard is on**, as ImGui's own id for it - so a walk
// that moves focus with Tab can say what it just pressed rather than counting
// how many times it pressed Tab and hoping the order has not changed.
uint32_t gs_ui_probe_focused(void);

// **Bring a panel to the front, by name.** Tab moves focus *within* a window
// and stops at its end: a tool with three panels open cannot be walked from the
// keyboard alone, which is not a fault in the tool - a player reaches for the
// mouse - but is a wall for anything walking it. Returns false if no window of
// that name is open.
bool gs_ui_probe_focus_window(const char *name);

// **Put ImGui back to a standing start, so one press cannot colour the next.**
// A walk restores the menu it is standing in by copying a value back over it,
// and that is the whole of the menu's state - but it is not the whole of the
// state on screen. A combo left open, or an item still held down, lives in
// ImGui's context rather than in gs_menu, and would otherwise be carried into
// the next thing pressed and read as its doing.
void gs_ui_probe_settle(void);

#ifdef __cplusplus
}
#endif

#endif

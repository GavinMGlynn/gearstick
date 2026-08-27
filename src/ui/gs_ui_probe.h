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

    // **Submitted, and scrolled out of sight.** A table with a scrollbar still
    // submits every row it holds, and ImGui reports each of them here before it
    // decides they are off-screen and drops them - so a library of forty tracks
    // comes back as forty rows whatever the window is tall enough to show. A
    // clipped item cannot be pressed: activating it by id sets a flag the item
    // never reads, because the item returns early. Counting one as a control
    // the walk pressed is counting a press that did not happen, which is how a
    // denominator quietly fills with things nobody can reach.
    //
    // This is ImGui's own test and not a likeness of it, down to the four ids
    // it keeps alive off-screen.
    bool     visible;

    // **All of it on screen, not merely some of it.** ImGui's clip test is an
    // overlap, so a button hanging half off the right-hand edge of a panel is
    // "visible" and is pressable by name - and is, to a person, a word cut in
    // two with nothing to click on. That is the fault the brush palette had and
    // the fault a gravity button had the day a slider beside it grew: the
    // control is there, the machine finds it, and nobody can see what it says.
    //
    // Only meaningful where nothing scrolls. A row at the top or bottom edge of
    // a list is cut by the list, and that is what a list is.
    bool     whole;

    // **Where it is, in pixels.** So a test can ask the question a person asks
    // by looking: is this control inside the panel it is drawn in? The clip
    // flags above cannot answer it - what the hook is handed has already been
    // clipped, so a button hanging off the right-hand edge arrives looking like
    // a narrower button that fits.
    float    x0, y0, x1, y1;

    // **A table's column heading is not a control.** ImGui submits the heading
    // row as items, reachable and not disabled and looking exactly like
    // buttons, and pressing one sorts the table - or does nothing at all, which
    // is what happens here, because none of these tables is sortable. Told
    // apart by what ImGui itself says the row is, rather than by a list of
    // names in a test that would go stale the day a column is renamed.
    bool     heading;
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

// **The wheel, over a named window.** A table taller than the panel holding it
// is not walked by pressing things: every row past the fold is submitted,
// dropped for being off-screen, and unreachable by any key. The wheel is how a
// person gets at them, so it is how the walk does - the mouse is put over the
// window and a wheel event queued, which is exactly what a backend does when
// somebody turns it. Nothing else in the walk uses a mouse.
//
// False if there is no window by that name.
bool gs_ui_probe_wheel(const char *window, float ticks);

// Where a named window is scrolled to, and how far it can go. `max` of zero
// means everything it holds is already on screen. False if there is no such
// window.
bool gs_ui_probe_scroll_at(const char *window, float *now, float *max);

// **Where a window is scrolled to on both axes, and how far it can go.** The
// vertical half of this used to be the whole of it, which was the same blind
// spot the panels themselves had: a window clamped to a screen too small for it
// scrolls what does not fit, and *sideways is a direction*. Four of the eight
// gravity buttons sat past the right-hand edge of the race setup screen at six
// hundred and forty across, with the test that was watching for exactly that
// looking only downwards.
bool gs_ui_probe_scroll_span(const char *window, float *x, float *y,
                             float *max_x, float *max_y);

// **Whether the window is showing that it can be scrolled.** Setting a scroll
// position from a test works whether or not there is a scrollbar on the window,
// so "the control can be reached by scrolling to it" is not on its own a claim
// about anything a person can do. This is the other half: a window that can
// move has to say so.
bool gs_ui_probe_scrollbars(const char *window, bool *x, bool *y);

// Put a window at a scroll position, so a test can ask what a person sees after
// they have scrolled there rather than only what they see on arrival.
bool gs_ui_probe_scroll_to(const char *window, float x, float y);

// Where a window ended up: position and size, in the same pixels as an item's
// rectangle.
bool gs_ui_probe_window_box(const char *window, float *x, float *y,
                            float *w, float *h);

// **Put a window back where it was.** ImGui remembers a window's position and
// size under its name for the rest of the process, so a test that moves or
// resizes one has changed the world for every test after it - the same hazard
// as resizing the screen and not putting it back, and harder to see because
// nothing in the next test mentions windows at all.
bool gs_ui_probe_place(const char *window, float x, float y, float w, float h);

// **Unfold a window somebody folded shut.** A collapsed window draws its title
// bar and nothing else, and ImGui remembers that under the window's name for
// the rest of the process - so one walk pressing a collapse arrow leaves every
// test after it walking a panel with nothing on it. True if it was folded.
bool gs_ui_probe_unfold(const char *window);

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

// gs_winmem.h - the window opens where it was left.
//
// A window that opens in the middle of the wrong monitor every morning is a
// small wrong thing a player fixes every single day. The geometry is written
// to a plain `key = value` text file in the preferences directory on the way
// out and read on the way in - text, not a blob, because window geometry is
// exactly the kind of thing somebody wants to look at or delete by hand when
// a window has gone somewhere strange.
//
// **Remembering is separate from daring to go back.** Monitors get unplugged,
// rearranged and fall asleep on the left, so a remembered position is only a
// suggestion until it is checked against the displays that exist *now* -
// otherwise the game opens somewhere nobody can see it and there is no mouse
// on earth that can drag it back. That check is a pure function over plain
// rectangles, so every arrangement of displays anyone can be hurt by is a
// test case rather than a monitor on a desk.
#ifndef GS_WINMEM_H
#define GS_WINMEM_H

#include <SDL3/SDL.h>

#define GS_WINMEM_FILE "window.txt"

typedef struct gs_winmem {
    int  x, y;        // where the window's top-left corner was
    int  w, h;        // how big it was
    bool placed;      // x and y are worth applying, not only the size
} gs_winmem;

// A first run: the given size, nowhere in particular - the window manager
// picks the spot, which is what it is best at when there is no history.
void gs_winmem_default(gs_winmem *m, int w, int h);

// Read `path`. Missing or unreadable leaves `m` alone and returns false,
// which is not an error - it is a first run. A file that parses but carries
// a size no window should be - below the smallest window the game runs in,
// or absurdly vast - is refused the same way, because applying it would be
// worse than forgetting it.
bool gs_winmem_load(gs_winmem *m, const char *path);

bool gs_winmem_save(const gs_winmem *m, const char *path);

// **Is enough of this window on one of these displays to grab?** The claim
// is about the title bar: at least GS_WINMEM_GRAB_W by GS_WINMEM_GRAB_H
// pixels of the window's top strip must land on a single display, because
// the top strip is what a mouse drags a window somewhere better by. A window
// remembered on a monitor that is gone fails this and opens at the window
// manager's choice instead.
#define GS_WINMEM_GRAB_W 64
#define GS_WINMEM_GRAB_H 24

bool gs_winmem_on_a_display(const gs_winmem *m, const SDL_Rect *displays,
                            int count);

#endif // GS_WINMEM_H

// gs_winmem.c - the window opens where it was left. See gs_winmem.h.
#include "platform/gs_winmem.h"

// The smallest window the game runs in, and a ceiling no real display is
// anywhere near. A remembered size outside these is a file that has been
// damaged or hand-edited into nonsense, and the answer to nonsense is the
// default, not a window two pixels tall.
#define GS_WINMEM_MIN_W 640
#define GS_WINMEM_MIN_H 480
#define GS_WINMEM_MAX_SIDE 32768

// Positions further out than this are refused at parse time. The on-a-display
// check is what really decides whether a position is usable; this only stops
// an integer overflow wearing a coordinate's clothes.
#define GS_WINMEM_FAR 1000000

void gs_winmem_default(gs_winmem *m, int w, int h) {
    m->x = 0;
    m->y = 0;
    m->w = w;
    m->h = h;
    m->placed = false;
}

// One `key = value` line. Own parsing rather than sscanf, so a damaged file
// is refused a field at a time and -Wconversion has nothing to say.
static bool gs_winmem_int(const char *s, int *out) {
    while (*s == ' ' || *s == '\t') s++;
    bool neg = *s == '-';
    if (neg) s++;
    if (*s < '0' || *s > '9') return false;

    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > GS_WINMEM_FAR) return false;
        s++;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    if (*s != '\0' && *s != '\n') return false;

    *out = neg ? (int)-v : (int)v;
    return true;
}

bool gs_winmem_load(gs_winmem *m, const char *path) {
    size_t len = 0;
    char *text = (char *)SDL_LoadFile(path, &len);
    if (text == nullptr) return false;

    gs_winmem got = { 0 };
    bool have_x = false, have_y = false, have_w = false, have_h = false;

    char *line = text;
    while (line != nullptr && *line != '\0') {
        char *next = SDL_strchr(line, '\n');
        if (next != nullptr) *next++ = '\0';

        char *eq = SDL_strchr(line, '=');
        if (eq != nullptr && *line != '#') {
            *eq = '\0';
            const char *value = eq + 1;
            // The key, trimmed of the spaces `key = value` writes.
            char *end = eq;
            while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
                *--end = '\0';
            }
            while (*line == ' ' || *line == '\t') line++;

            if (SDL_strcmp(line, "x") == 0) {
                have_x = gs_winmem_int(value, &got.x);
            } else if (SDL_strcmp(line, "y") == 0) {
                have_y = gs_winmem_int(value, &got.y);
            } else if (SDL_strcmp(line, "w") == 0) {
                have_w = gs_winmem_int(value, &got.w);
            } else if (SDL_strcmp(line, "h") == 0) {
                have_h = gs_winmem_int(value, &got.h);
            }
            // An unknown key is somebody else's field: ignored, not an error,
            // so a file written by a newer build still opens this one's
            // window where it was.
        }
        line = next;
    }
    SDL_free(text);

    // The size has to be one a window can be; the file is refused whole
    // rather than half-applied, because half a memory is a window the size
    // it was in a place it was not.
    if (!have_w || !have_h) return false;
    if (got.w < GS_WINMEM_MIN_W || got.h < GS_WINMEM_MIN_H) return false;
    if (got.w > GS_WINMEM_MAX_SIDE || got.h > GS_WINMEM_MAX_SIDE) return false;

    m->w = got.w;
    m->h = got.h;
    m->placed = have_x && have_y;
    if (m->placed) {
        m->x = got.x;
        m->y = got.y;
    }
    return true;
}

bool gs_winmem_save(const gs_winmem *m, const char *path) {
    char text[256];
    const int n = SDL_snprintf(
        text, sizeof text,
        "# where the gearstick window was when the game last closed.\n"
        "# delete this file if the window has gone somewhere strange.\n"
        "x = %d\ny = %d\nw = %d\nh = %d\n",
        m->x, m->y, m->w, m->h);
    if (n <= 0 || (size_t)n >= sizeof text) return false;
    return SDL_SaveFile(path, text, (size_t)n);
}

bool gs_winmem_on_a_display(const gs_winmem *m, const SDL_Rect *displays,
                            int count) {
    if (!m->placed) return false;

    // The strip a mouse drags a window by: the top GS_WINMEM_GRAB_H pixels.
    // Checked against each display alone rather than against their union,
    // because a corner of one monitor plus a corner of another is not a
    // place anybody can grab.
    const SDL_Rect strip = { m->x, m->y, m->w, GS_WINMEM_GRAB_H };

    for (int i = 0; i < count; i++) {
        SDL_Rect in;
        if (!SDL_GetRectIntersection(&strip, &displays[i], &in)) continue;
        if (in.w >= GS_WINMEM_GRAB_W && in.h >= GS_WINMEM_GRAB_H) return true;
    }
    return false;
}

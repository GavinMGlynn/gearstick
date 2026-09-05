// gs_default.c - the server the game points at by default. See gs_default.h.
#include "platform/gs_default.h"
#include "platform/gs_paths.h"

#include <SDL3/SDL.h>

static bool gs_hex_byte(const char *p, uint8_t *out) {
    unsigned byte = 0;
    for (int half = 0; half < 2; half++) {
        const char ch = p[half];
        const unsigned digit = (ch >= '0' && ch <= '9') ? (unsigned)(ch - '0')
                             : (ch >= 'a' && ch <= 'f') ? (unsigned)(ch - 'a' + 10)
                             : (ch >= 'A' && ch <= 'F') ? (unsigned)(ch - 'A' + 10)
                                                        : 16u;
        if (digit > 15u) return false;
        byte = byte * 16u + digit;
    }
    *out = (uint8_t)byte;
    return true;
}

static bool gs_blank(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r';
}

bool gs_default_server_parse(const char *text, gs_default_server *out) {
    if (text == nullptr || out == nullptr) return false;
    SDL_zerop(out);

    const char *p = text;
    bool named = false;
    while (*p != '\0') {
        // One line at a time. Blank and comment lines are skipped; the first
        // line with anything on it has to be the whole address, and a second
        // such line is a file that says two things, which is no default.
        const char *end = SDL_strchr(p, '\n');
        const size_t len = end != nullptr ? (size_t)(end - p) : SDL_strlen(p);
        const char *q = p;
        while (q < p + len && gs_blank(*q)) q++;
        if (q == p + len || *q == '#') {
            p = end != nullptr ? end + 1 : p + len;
            continue;
        }
        if (named) return false;

        // The host: up to the next blank.
        const char *h = q;
        while (q < p + len && !gs_blank(*q)) q++;
        const size_t hn = (size_t)(q - h);
        if (hn == 0 || hn >= GS_DEFAULT_HOST) return false;
        SDL_memcpy(out->host, h, hn);
        out->host[hn] = '\0';

        // The port: digits, 1 to 65535.
        while (q < p + len && gs_blank(*q)) q++;
        unsigned port = 0;
        const char *d = q;
        while (q < p + len && *q >= '0' && *q <= '9') {
            port = port * 10u + (unsigned)(*q - '0');
            if (port > 65535u) return false;
            q++;
        }
        if (q == d || port == 0) return false;
        out->port = (uint16_t)port;

        // The key: exactly 64 hex characters, and nothing after them but
        // blanks.
        while (q < p + len && gs_blank(*q)) q++;
        if ((size_t)(p + len - q) < (size_t)GS_NOISE_KEY_BYTES * 2u) return false;
        for (int k = 0; k < GS_NOISE_KEY_BYTES; k++) {
            if (!gs_hex_byte(q + k * 2, &out->key[k])) return false;
        }
        q += (size_t)GS_NOISE_KEY_BYTES * 2u;
        while (q < p + len && gs_blank(*q)) q++;
        if (q != p + len) return false;

        named = true;
        p = end != nullptr ? end + 1 : p + len;
    }
    return named;
}

// The whole of a small text file, or null. Never more than a few kilobytes:
// a server file with a megabyte in it is not a server file.
static char *gs_read_small(const char *path) {
    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    if (data == nullptr) return nullptr;
    if (n > 4096) {
        SDL_free(data);
        return nullptr;
    }
    return (char *)data;      // SDL_LoadFile terminates it
}

bool gs_default_server_load(gs_default_server *out, char *why, size_t why_cap) {
    char path[1024];

    // The player's own first: a file beside their drivers and records.
    SDL_snprintf(path, sizeof path, "%s%s", gs_pref_dir(), GS_DEFAULT_SERVER_FILE);
    char *text = gs_read_small(path);
    if (text != nullptr) {
        const bool ok = gs_default_server_parse(text, out);
        SDL_free(text);
        if (ok) {
            if (why != nullptr) SDL_snprintf(why, why_cap, "from %s", path);
            return true;
        }
        // A file of theirs that names nothing is not a reason to ignore the
        // shipped one: a player who blanked it wants the default back.
    }

    gs_asset_path(path, sizeof path, GS_DEFAULT_SERVER_FILE);
    text = gs_read_small(path);
    if (text != nullptr) {
        const bool ok = gs_default_server_parse(text, out);
        SDL_free(text);
        if (ok) {
            if (why != nullptr) SDL_snprintf(why, why_cap, "from %s", path);
            return true;
        }
    }

    if (why != nullptr) {
        SDL_snprintf(why, why_cap, "no server is named in %s%s or in the shipped %s",
                     gs_pref_dir(), GS_DEFAULT_SERVER_FILE, GS_DEFAULT_SERVER_FILE);
    }
    SDL_zerop(out);
    return false;
}

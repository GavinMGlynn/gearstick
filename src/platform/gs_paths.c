// gs_paths.c - see gs_paths.h.

#include <SDL3/SDL.h>

#include "platform/gs_paths.h"

static char gs_assets[1024];
static char gs_prefs[1024];

static bool gs_dir_exists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

const char *gs_assets_dir(void) {
    if (gs_assets[0] != '\0') return gs_assets;

    const char *base = SDL_GetBasePath();
    if (base != nullptr) {
        // Beside the executable, which is how a package is laid out. On macOS
        // SDL_GetBasePath returns the bundle's resource path, so this is
        // Contents/Resources/assets - see the install rules in CMakeLists.txt.
        SDL_snprintf(gs_assets, sizeof gs_assets, "%sassets", base);
        if (gs_dir_exists(gs_assets)) return gs_assets;

        // One level up, which is where a build tree puts it.
        SDL_snprintf(gs_assets, sizeof gs_assets, "%s../assets", base);
        if (gs_dir_exists(gs_assets)) return gs_assets;
    }

#ifdef GS_SOURCE_ASSETS
    // The source tree, so running out of build/ works with no install step.
    SDL_snprintf(gs_assets, sizeof gs_assets, "%s", GS_SOURCE_ASSETS);
    if (gs_dir_exists(gs_assets)) return gs_assets;
#endif

    SDL_snprintf(gs_assets, sizeof gs_assets, "%sassets", base != nullptr ? base : "./");
    return gs_assets;
}

const char *gs_pref_dir(void) {
    if (gs_prefs[0] != '\0') return gs_prefs;

    char *p = SDL_GetPrefPath("gearstick", "gearstick");
    if (p == nullptr) return nullptr;

    SDL_snprintf(gs_prefs, sizeof gs_prefs, "%s", p);
    SDL_free(p);
    return gs_prefs;
}

char *gs_asset_path(char *out, size_t cap, const char *name) {
    SDL_snprintf(out, cap, "%s%c%s", gs_assets_dir(), '/', name);
    return out;
}

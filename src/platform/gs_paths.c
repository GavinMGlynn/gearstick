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

    // **Somewhere else, when something says so** - the same override
    // `gs_pref_dir` carries, and for the same reason. What the game ships is an
    // input to the library the player ends up with, so the rule about *which*
    // shipped tracks survive a new version cannot be checked without staging a
    // different set of them. Pointing this at a copy is how that is done
    // without a test writing into the repository it is testing.
    const char *set = SDL_getenv("GEARSTICK_ASSETS_DIR");
    if (set != nullptr && set[0] != '\0' && gs_dir_exists(set)) {
        SDL_snprintf(gs_assets, sizeof gs_assets, "%s", set);
        return gs_assets;
    }

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

    // **Somewhere else, when something says so.**
    //
    // The suite presses every control in the construction set, and two of those
    // controls save a track and the key bindings into the preferences
    // directory. Pointing `HOME` at a throwaway inside the build tree keeps
    // `ctest` out of a real player's files - on Linux. On macOS it does
    // nothing: SDL asks the platform where a user's things live and the
    // platform answers from the password database, not from the environment.
    // So the run wrote to the actual `~/Library/Application Support`, and the
    // test that says out loud where the suite is allowed to write went red on
    // macOS and stayed red.
    //
    // An override that every platform reads the same way is the fix. A portable
    // install can use it too - a copy on a memory stick that keeps its
    // preferences beside itself.
    const char *set = SDL_getenv("GEARSTICK_PREF_DIR");
    if (set != nullptr && set[0] != '\0') {
        size_t n = SDL_strlen(set);
        bool ends = n > 0 && (set[n - 1] == '/' || set[n - 1] == '\\');
        // With the trailing separator SDL_GetPrefPath promises, because
        // everything that builds a filename from this appends straight onto it.
        SDL_snprintf(gs_prefs, sizeof gs_prefs, "%s%s", set, ends ? "" : "/");
        SDL_CreateDirectory(gs_prefs);
        return gs_prefs;
    }

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

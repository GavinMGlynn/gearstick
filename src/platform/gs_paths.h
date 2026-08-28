// gs_paths.h - where the art is, and where the player's own things go.
//
// A package is a folder somebody unpacks and runs, so the shipped layout is the
// executable at the top and `assets/` beside it. Running straight out of the
// build tree has to work too, without an install step, or the edit-build-run
// loop grows one.
#ifndef GS_PATHS_H
#define GS_PATHS_H

#include "core/gs_common.h"

// Absolute path to the assets directory, probed once. Never null; if nothing is
// found it returns the last place it looked, so the failure names a path.
// `GEARSTICK_ASSETS_DIR` overrides it, the way `GEARSTICK_PREF_DIR` overrides
// the other one.
const char *gs_assets_dir(void);

// Where the player's tracks, replays and settings live - the OS's preferences
// directory, created if it is not there. Null only if the OS refuses.
const char *gs_pref_dir(void);

// Join the assets directory and `name` into `out`. Returns out.
char *gs_asset_path(char *out, size_t cap, const char *name);

#endif // GS_PATHS_H

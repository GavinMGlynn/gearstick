#ifndef GS_SANDBOX_H
#define GS_SANDBOX_H

// **Where a test is allowed to write, held by the binary rather than by how it
// was started.**
//
// The suite presses every control in the construction set, and two of those
// save a track and the key bindings into the preferences directory. That was
// pointed at a throwaway by setting environment variables in `ctest`, which is
// right up to the moment somebody runs a test binary directly - which is what
// anybody debugging one does, all afternoon, and there is nothing in the
// binary that knows it has left the sandbox behind.
//
// So every test main says it here too. `overwrite` is zero, so a value already
// in the environment wins: CMake still chooses the directory, and a bare run of
// the binary gets the same one instead of a player's real files.
//
// GS_TEST_HOME is a compile definition set by CMakeLists.txt, and
// `no_test_writes_where_a_player_keeps_their_things` checks the two agree.
//
// Not included by the test binaries that link no SDL - which is most of the
// simulation's own. A binary with no SDL in it cannot call `gs_pref_dir`, so
// there is nothing in it to keep out of anybody's files, and pulling SDL in to
// say so would break the layering the whole project is built on.

#include <SDL3/SDL_stdinc.h>

static inline void gs_sandbox(void) {
    SDL_setenv_unsafe("GEARSTICK_PREF_DIR", GS_TEST_HOME "/prefs", 0);
}

#endif

cmake_minimum_required(VERSION 3.28)

project(gearstick
    VERSION     0.1.0
    DESCRIPTION "Gearstick - a Racing Destruction Set-style construction racer in C23 + SDL3"
    # C is the project. CXX is here for exactly one reason: Dear ImGui, which
    # the editor's panels are built on and which is C++ underneath its C API.
    # Nothing of ours is written in it - see the gearstick_ui target below, and
    # docs/PROJECT_STATUS.md for why the exception was worth making.
    LANGUAGES   C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(GEARSTICK_USE_SYSTEM_SDL "Use find_package(SDL3) instead of ext/sdl" OFF)
option(GEARSTICK_TESTS          "Build the unit tests"                      ON)

# Warnings are errors, in **every** build type - not debug and CI alone.
# A warning that is only fatal somewhere else is a warning that gets committed
# here and discovered by someone else's build.
#   cmake -B build -DGEARSTICK_WERROR=OFF
option(GEARSTICK_WERROR         "Treat compiler warnings as errors"         ON)
option(GEARSTICK_ASAN           "Build with address+UB sanitizers"          OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Dear ImGui's floor. Ours is C23; this applies to the vendored C++ alone.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # so clangd/IDEs see -std=c23

include(Platform)
include(CompilerWarnings)
include(Layering)

gs_check_layering()

# ---------------------------------------------------------------------------
# The simulation
#
# **A standalone C library that links nothing.** Not SDL, not libm, not the
# renderer. Everything good in this design is downstream of that: input-log
# replays that reproduce a race exactly, the editor's background ghost, the
# headless sweep that answers "is this track completable between 0.4g and 1.8g"
# in CI, and rollback netcode later. See cmake/Layering.cmake, which enforces it.
# ---------------------------------------------------------------------------
add_library(gearstick_sim STATIC
    src/core/gs_clock.c
    src/core/gs_edit.c
    src/core/gs_fixed.c
    src/core/gs_track.c
    src/core/gs_vehicle.c
    src/core/gs_sim.c
    src/core/gs_replay.c)

# One include root, so every #include names the layer it crosses:
#   #include "core/gs_sim.h"   not   #include "../core/gs_sim.h"
target_include_directories(gearstick_sim PUBLIC src)
gs_configure(gearstick_sim)

# ---------------------------------------------------------------------------
# Dependencies
#
# SDL is a pinned submodule under ext/ rather than a FetchContent download, so a
# clone records the exact commit it builds against and an offline build works.
# See ext/README.md. Nothing below this line is linked into gearstick_sim.
#
# SDL is the only dependency, and there is deliberately no image library yet:
# the terrain is generated geometry rather than a tile atlas, and until there
# are vehicle sprites to decode, an image decoder would be a dependency on three
# platforms for a file type this game does not ship.
# ---------------------------------------------------------------------------
if(GEARSTICK_USE_SYSTEM_SDL)
    find_package(SDL3 3.2 REQUIRED CONFIG)
    set(GS_SDL SDL3::SDL3)
    message(STATUS "gearstick: using system SDL3 ${SDL3_VERSION}")
else()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/sdl/CMakeLists.txt")
        message(FATAL_ERROR
            "ext/sdl is empty - the submodules are not checked out.\n"
            "  git submodule update --init --depth 1 ext/sdl\n"
            "Or configure with -DGEARSTICK_USE_SYSTEM_SDL=ON to use an "
            "installed SDL3 instead.")
    endif()

    set(SDL_SHARED       OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)

    # Four pads on one couch is the point of this game, and an Xbox pad speaks a
    # vendor-specific protocol: on Linux it needs either the kernel's xpad or
    # SDL talking to it through libusb. WSL2 has no xpad, so without this the
    # pad is invisible there. If libusb is missing at configure time SDL turns
    # it back off by itself.
    if(UNIX AND NOT APPLE)
        set(SDL_HIDAPI_LIBUSB ON CACHE BOOL "" FORCE)
    endif()

    add_subdirectory(ext/sdl EXCLUDE_FROM_ALL)
    set(GS_SDL SDL3::SDL3-static)

    message(STATUS "gearstick: SDL3 from the ext/ submodule")
endif()

# ---------------------------------------------------------------------------
# The editor's UI - Dear ImGui, and the generated C API over it
#
# **The one C++ in the project, and none of it is ours.** Palettes, property
# panels, validation output and live tuning dials are what C is worst at and
# what kills projects shaped like this one; immediate mode removes widget
# lifetimes and state synchronisation entirely, which is what makes it right for
# a tool even where it would be wrong for a shipping interface.
#
# Deliberately NOT passed through gs_configure(): vendored and generated code is
# built under its own settings, exactly like SDL. Warnings we cannot fix are
# warnings people learn to scroll past.
#
# src/ui/dcimgui/ is generated and committed - see its README, and
# tools/make_imgui_bindings.py.
# ---------------------------------------------------------------------------
add_library(gearstick_ui STATIC
    ext/imgui/imgui.cpp
    ext/imgui/imgui_draw.cpp
    ext/imgui/imgui_tables.cpp
    ext/imgui/imgui_widgets.cpp
    # imgui_demo.cpp is not optional despite the name: ShowStyleSelector and
    # ShowUserGuide live in it, the generated bindings export both, and leaving
    # it out is an undefined reference at link time rather than a smaller
    # binary. It brings the demo window along, which is worth having while
    # building an editor anyway.
    ext/imgui/imgui_demo.cpp
    ext/imgui/backends/imgui_impl_sdl3.cpp
    ext/imgui/backends/imgui_impl_sdlrenderer3.cpp
    src/ui/dcimgui/dcimgui.cpp
    src/ui/dcimgui/backends/dcimgui_impl_sdl3.cpp
    src/ui/dcimgui/backends/dcimgui_impl_sdlrenderer3.cpp)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/imgui/imgui.cpp")
    message(FATAL_ERROR
        "ext/imgui is empty - the submodules are not checked out.\n"
        "  git submodule update --init --depth 1 ext/imgui")
endif()

target_include_directories(gearstick_ui PUBLIC
    ext/imgui
    ext/imgui/backends
    src/ui/dcimgui
    src/ui/dcimgui/backends)
target_link_libraries(gearstick_ui PUBLIC ${GS_SDL})
set_target_properties(gearstick_ui PROPERTIES C_VISIBILITY_PRESET hidden)

# ---------------------------------------------------------------------------
# The shell - everything the simulation deliberately cannot do
# ---------------------------------------------------------------------------
add_library(gearstick_shell STATIC
    src/ui/gs_editor.c
    src/gfx/gs_iso.c
    src/gfx/gs_render.c
    src/platform/gs_paths.c
    src/platform/gs_input.c)

target_include_directories(gearstick_shell PUBLIC src)
target_link_libraries(gearstick_shell PUBLIC gearstick_sim gearstick_ui ${GS_SDL})

# Where the art lives when running straight out of the build tree, so the
# ordinary edit-build-run loop needs no install step. See src/platform/gs_paths.c.
target_compile_definitions(gearstick_shell PRIVATE
    GS_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")

gs_configure(gearstick_shell)

# ---------------------------------------------------------------------------
# Frontends
# ---------------------------------------------------------------------------
add_executable(gearstick src/frontend/game/main.c)
target_link_libraries(gearstick PRIVATE gearstick_shell)
gs_configure(gearstick)

# Windows: GUI subsystem so no console window pops up. SDL_MAIN_USE_CALLBACKS
# in main.c supplies WinMain via SDL3/SDL_main.h.
set_target_properties(gearstick PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    OFF
    C_VISIBILITY_PRESET hidden)

# The headless driver. **Links gearstick_sim and nothing else** - no SDL, no
# window, no audio device. That is not a convenience, it is the proof: if this
# stops linking, the simulation has grown a dependency on being watched.
add_executable(gearstick_cli src/frontend/cli/main.c)
target_link_libraries(gearstick_cli PRIVATE gearstick_sim)
gs_configure(gearstick_cli)
set_target_properties(gearstick_cli PROPERTIES C_VISIBILITY_PRESET hidden)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GEARSTICK_TESTS)
    enable_testing()
    add_executable(gearstick_tests tests/test_gearstick.c)
    target_link_libraries(gearstick_tests PRIVATE gearstick_sim)

    # The tests link libm and the simulation does not. That asymmetry is the
    # point: checking the fixed-point trigonometry against double precision is
    # exactly the job that needs a maths library, and doing it out here is what
    # lets the rule in src/core/ be absolute.
    if(NOT MSVC)
        target_link_libraries(gearstick_tests PRIVATE m)
    endif()

    gs_configure(gearstick_tests)

    add_test(NAME gearstick COMMAND gearstick_tests)
    set_tests_properties(gearstick PROPERTIES TIMEOUT 120)

    # The renderer's own tests, which need a window system and therefore link
    # gearstick_shell. Kept a separate executable from the simulation tests on
    # purpose: those link gearstick_sim and nothing else, and that is the
    # standing proof the simulation does not know it is being looked at. One
    # test binary linking both would quietly retire the proof.
    add_executable(gearstick_render_tests tests/test_render.c)
    target_link_libraries(gearstick_render_tests PRIVATE gearstick_shell)
    gs_configure(gearstick_render_tests)

    add_test(NAME gearstick_render COMMAND gearstick_render_tests)
    # No display, no audio device, no pads - and the software renderer, so the
    # pixels are the same ones on every platform's CI.
    set_tests_properties(gearstick_render PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy"
        TIMEOUT 180)

    # A track through a real file, not just a buffer: the format is what a
    # shared track travels in, so "it round-trips in memory" is not the claim
    # that matters.
    add_test(NAME gearstick_track_file
             COMMAND gearstick_cli track round_trip.gstrack)
    set_tests_properties(gearstick_track_file PROPERTIES TIMEOUT 60)

    # Every way a route can be broken is refused, and named.
    add_test(NAME gearstick_validate COMMAND gearstick_cli validate)
    set_tests_properties(gearstick_validate PROPERTIES TIMEOUT 60)

    # Every vehicle is best at something. A roster where one machine wins
    # everything is a roster of one and five decorations, and this is what
    # noticed that the first one was.
    add_test(NAME gearstick_roster COMMAND gearstick_cli roster)
    set_tests_properties(gearstick_roster PROPERTIES TIMEOUT 120)

    # The golden replay: an input log in, a state hash out. This is the test
    # that fails when a "harmless" change to the physics quietly rewrites every
    # ghost time and every shared replay in existence.
    add_test(NAME gearstick_golden_replay
             COMMAND gearstick_cli selftest --verify)
    set_tests_properties(gearstick_golden_replay PROPERTIES TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# Installing, and packaging
#
# **A package is a folder somebody unpacks and runs**, so the layout is the one
# src/platform/gs_paths.c probes: the executables at the top and assets/ beside
# them.
# ---------------------------------------------------------------------------
set(GS_DOCS README.md LICENSE)

if(APPLE)
    set_target_properties(gearstick PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME          "gearstick"
        MACOSX_BUNDLE_GUI_IDENTIFIER       "dev.gavin.gearstick"
        MACOSX_BUNDLE_BUNDLE_VERSION       "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT            "GPL-3.0-or-later")
    install(TARGETS gearstick BUNDLE DESTINATION .)
    # Inside Resources/assets, not Resources itself: SDL_GetBasePath returns a
    # bundle's *resourcePath*, so the `<base>/assets` the probe in gs_paths.c
    # looks for is Contents/Resources/assets.
    install(DIRECTORY assets/ DESTINATION "gearstick.app/Contents/Resources/assets")
    install(TARGETS gearstick_cli RUNTIME DESTINATION .)
    install(FILES ${GS_DOCS} DESTINATION .)
else()
    install(TARGETS gearstick gearstick_cli RUNTIME DESTINATION .)
    install(DIRECTORY assets/ DESTINATION assets)
    install(FILES ${GS_DOCS} DESTINATION .)
endif()

# ---------------------------------------------------------------------------
# CPack
# ---------------------------------------------------------------------------
# What a release calls itself, which is not always the project version - a beta
# of 0.1.0 is still 0.1.0, and somebody with the file in their downloads folder
# needs to be able to tell. Set by the release job from the tag.
set(GEARSTICK_RELEASE "${PROJECT_VERSION}" CACHE STRING
    "Version the packages are named for, e.g. 0.1.0-beta.1")

set(CPACK_PACKAGE_NAME          "gearstick")
set(CPACK_PACKAGE_VENDOR        "Gavin Glynn")
set(CPACK_PACKAGE_VERSION       "${GEARSTICK_RELEASE}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY   "Gearstick")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM      SHA256)
set(CPACK_STRIP_FILES           ON)
set(CPACK_VERBATIM_VARIABLES    ON)
set(CPACK_PACKAGE_FILE_NAME "gearstick-${GEARSTICK_RELEASE}-${GS_PLATFORM}")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ;DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Gearstick ${PROJECT_VERSION}")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)

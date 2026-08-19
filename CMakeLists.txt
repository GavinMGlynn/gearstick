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

# **This is not our C++ standard, because there is no first-party C++.** Every
# .cpp compiled here is Dear ImGui's; our side of that boundary is the generated
# C API, which is C23 like everything else. So this is the standard somebody
# else's code is compiled with, and the two numbers have no relationship to
# maintain - C and C++ standards are independent and there is no interop to win.
#
# C++20 rather than the 17 it started at because it costs nothing and is
# supported outright by all four toolchains this project builds on. Checked
# rather than assumed: ImGui compiles at C++20 with no errors and no new
# warnings. C++23 is deliberately not taken - AppleClang and MSVC coverage is
# patchier, and the payoff for a project that writes no C++ is exactly zero.
set(CMAKE_CXX_STANDARD 20)
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
    src/core/gs_ai.c
    src/core/gs_blake2s.c
    src/core/gs_analyse.c
    src/core/gs_generate.c
    src/core/gs_ghost.c
    src/core/gs_library.c
    src/core/gs_net.c
    src/core/gs_profile.c
    src/core/gs_records.c
    src/core/gs_pack.c
    src/core/gs_share.c
    src/core/gs_stunts.c
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
# The wire - SDL_net
#
# **Datagrams only, and nothing above them.** The netcode is rollback, which
# needs a socket that will drop and reorder and nothing else: no reliability, no
# ordering, no connection state, because every one of those things is a round
# trip and rollback exists so that nobody waits for a round trip. Loss is
# absorbed by sending the last thirty-two ticks of input in every packet.
#
# Nothing in src/core/ can see this. The rollback session itself produces and
# consumes byte arrays and has never heard of a socket, which is what lets a
# whole race under 200 ms of latency and 12% loss run inside a unit test.
# ---------------------------------------------------------------------------
set(GS_SDL_NET "")
if(GEARSTICK_USE_SYSTEM_SDL)
    # Quietly, and not REQUIRED: SDL3_net is new enough that no distribution
    # packages it yet, so "use the system SDL" cannot reasonably mean "and fail
    # if the system has no SDL_net". The submodule is the fallback.
    find_package(SDL3_net 3.0 QUIET CONFIG)
    if(SDL3_net_FOUND)
        set(GS_SDL_NET SDL3_net::SDL3_net)
        message(STATUS "gearstick: using system SDL3_net")
    endif()
endif()

if(NOT GS_SDL_NET)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/sdl_net/CMakeLists.txt")
        message(FATAL_ERROR
            "ext/sdl_net is empty - the submodules are not checked out.\n"
            "  git submodule update --init --depth 1 ext/sdl_net")
    endif()

    set(SDLNET_TESTS   OFF CACHE BOOL "" FORCE)
    set(SDLNET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(SDLNET_INSTALL OFF CACHE BOOL "" FORCE)

    # SDL_net chooses static or shared from BUILD_SHARED_LIBS rather than from
    # an option of its own, so it is set around the include and put back. A
    # shared SDL_net would be a second file to ship beside a game that is
    # otherwise one executable.
    set(GS_BUILD_SHARED_WAS ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(ext/sdl_net EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS ${GS_BUILD_SHARED_WAS})

    # SDL_net asks for c_std_99, which CMake renders as a *strict* -std=c99, and
    # strict C99 hides everything POSIX - including EAI_SYSTEM and gai_strerror,
    # which SDL_net itself calls. The extensions have to be back on for it to
    # compile at all. This applies to SDL_net's own target and to nothing of
    # ours: gearstick is built as strict C23 and stays that way.
    if(TARGET SDL3_net-static)
        set_target_properties(SDL3_net-static PROPERTIES C_EXTENSIONS ON)
    endif()

    set(GS_SDL_NET SDL3_net::SDL3_net)

    message(STATUS "gearstick: SDL3_net from the ext/ submodule")
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
    src/ui/gs_hud.c
    src/ui/gs_menu.c
    src/ui/gs_style.c
    src/gfx/gs_iso.c
    src/audio/gs_audio.c
    src/audio/gs_music.c
    src/gfx/gs_meshes.c
    src/gfx/gs_render.c
    src/platform/gs_bind.c
    src/platform/gs_paths.c
    src/platform/gs_input.c
    src/platform/gs_wire.c
    src/net/gs_proto.c
    src/net/gs_carrier.c)

target_include_directories(gearstick_shell PUBLIC src)
target_link_libraries(gearstick_shell PUBLIC gearstick_sim gearstick_ui ${GS_SDL}
    ${GS_SDL_NET})

# Where the art lives when running straight out of the build tree, so the
# ordinary edit-build-run loop needs no install step. See src/platform/gs_paths.c.
target_compile_definitions(gearstick_shell PRIVATE
    GS_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")

# Where the player's guide is, so a test can read it and check that what it
# tells people to press is what the game actually listens for. A guide that has
# drifted from the code is worse than no guide: it sends somebody to press a key
# that does nothing and lets them conclude the game is broken.
target_compile_definitions(gearstick_shell PUBLIC
    GS_SOURCE_DOCS="${CMAKE_CURRENT_SOURCE_DIR}/docs"
    # The package name this platform actually produces, so a test can check the
    # release notes describe files that exist rather than files somebody
    # remembered. Getting this wrong is silent: the document simply describes a
    # download nobody has.
    GS_PACKAGE_PLATFORM="${GS_PLATFORM}")

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
# The stock tracks, written as data. Links the simulation and nothing else, like
# every other tool that touches the formats.
add_executable(gearstick_make_tracks tools/make_tracks.c)
target_link_libraries(gearstick_make_tracks PRIVATE gearstick_sim)
gs_configure(gearstick_make_tracks)

add_executable(gearstick_cli src/frontend/cli/main.c)
target_link_libraries(gearstick_cli PRIVATE gearstick_sim)
gs_configure(gearstick_cli)
set_target_properties(gearstick_cli PROPERTIES C_VISIBILITY_PRESET hidden)

# ---------------------------------------------------------------------------
# The server - the meeting point for online races
#
# **Headless on purpose.** It links SDL because SDL_net needs it, and no part of
# the renderer or the UI, because a server that needed a display could not run
# where servers run. It does not link the simulation either, yet: it is a
# librarian and a referee rather than a player, and the day it re-races a
# submitted replay to check a time is the day it gains gearstick_sim.
# ---------------------------------------------------------------------------
option(GEARSTICK_SERVER "Build gearstick_server" ON)
option(GEARSTICK_SQLITE_AMALGAMATION
       "Always fetch SQLite rather than using a system copy" OFF)

if(GEARSTICK_SERVER)
include(cmake/Sqlite.cmake)
gs_add_sqlite()

add_executable(gearstick_server
    src/frontend/server/main.c
    src/net/gs_proto.c
    src/net/gs_carrier.c
    src/net/gs_store.c
    src/net/gs_verify.c
    # Where the shipped library is. The server has no window and no art, but it
    # does ship a database of tracks, and finding it is the same problem the
    # game already solved.
    src/platform/gs_paths.c)
# The carrier rebuilds a track, so the server needs the track format - and with
# it the simulation, which it will need anyway the day it verifies a lap time.
target_include_directories(gearstick_server PRIVATE src)
# The same build-tree assets the game finds, so a server run straight out of the
# build tree ships with the same library a packaged one does.
target_compile_definitions(gearstick_server PRIVATE
    GS_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
target_link_libraries(gearstick_server PRIVATE ${GS_SDL} ${GS_SDL_NET}
    gearstick_sim gearstick_sqlite)
gs_configure(gearstick_server)
set_target_properties(gearstick_server PROPERTIES C_VISIBILITY_PRESET hidden)

# The database a server ships with. Built through the same gs_store the server
# uses, so there is no second copy of the schema to disagree with.
add_executable(gearstick_make_store tools/make_store.c src/net/gs_store.c)
target_include_directories(gearstick_make_store PRIVATE src)
target_link_libraries(gearstick_make_store PRIVATE gearstick_sim gearstick_sqlite)
gs_configure(gearstick_make_store)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(GEARSTICK_TESTS)
    enable_testing()
    add_executable(gearstick_tests
        tests/test_gearstick.c
        src/net/gs_proto.c
        src/net/gs_carrier.c
        src/net/gs_verify.c)
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

    # The netcode over a real socket rather than a simulated link. Everything
    # about *rollback* is checked against a link whose latency and loss are
    # chosen, because a failure there reproduces; this checks the thing that
    # cannot be simulated - that a host finds a peer it was never told about and
    # that two ends agree once real datagrams carry the race.
    add_executable(gearstick_wire_tests tests/test_wire.c)
    target_link_libraries(gearstick_wire_tests PRIVATE gearstick_shell)
    gs_configure(gearstick_wire_tests)

    # The lobby, with the server as a real process on the loopback. It is a
    # separate program in real use and it is a separate program here - starting
    # it in-process would test a server nobody runs.
    # Links the shell, because half of what this checks is the client the game
    # actually ships rather than a client written for the test.
    add_executable(gearstick_server_tests
        tests/test_server.c
        src/net/gs_verify.c)
    target_link_libraries(gearstick_server_tests PRIVATE gearstick_shell)
    gs_configure(gearstick_server_tests)
    add_dependencies(gearstick_server_tests gearstick_server)

    # What the server remembers. Links no SDL: the store is SQLite and the
    # track format, and neither of those has ever needed a window.
    add_executable(gearstick_store_tests
        tests/test_store.c
        src/net/gs_store.c)
    target_include_directories(gearstick_store_tests PRIVATE src)
    # gearstick_sim so the shipped library's tracks can be read back as tracks
    # rather than trusted as blobs; it links nothing, so this stays SDL-free.
    target_link_libraries(gearstick_store_tests PRIVATE gearstick_sqlite gearstick_sim)
    target_compile_definitions(gearstick_store_tests PRIVATE
        GS_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    gs_configure(gearstick_store_tests)

    add_test(NAME gearstick_store COMMAND gearstick_store_tests)
    set_tests_properties(gearstick_store PROPERTIES TIMEOUT 120)

    add_test(NAME gearstick_server COMMAND gearstick_server_tests)
    set_tests_properties(gearstick_server PROPERTIES TIMEOUT 180)

    add_test(NAME gearstick_wire COMMAND gearstick_wire_tests)
    set_tests_properties(gearstick_wire PROPERTIES TIMEOUT 120)

    # The synthesiser, measured rather than listened to. A listening test is
    # the right verification and is not one CI can run; this checks the things a
    # listener would notice and a forgotten listening test would not - that the
    # note follows the gearbox, that the surface reaches the tyres, that a jump
    # takes the tyre noise with it, and that nothing ever leaves the [-1, 1] a
    # speaker cone lives in.
    add_executable(gearstick_audio_tests tests/test_audio.c)
    target_link_libraries(gearstick_audio_tests PRIVATE gearstick_shell)
    gs_configure(gearstick_audio_tests)

    add_test(NAME gearstick_audio COMMAND gearstick_audio_tests)
    # No driver named at all: these tests open no device, because a callback
    # thread pulling on the mixer makes what they measure depend on when it
    # fired. See gs_audio_open_silent.
    set_tests_properties(gearstick_audio PROPERTIES TIMEOUT 120)

    add_test(NAME gearstick_render COMMAND gearstick_render_tests)
    # No display, no audio device, no pads - and the software renderer, so the
    # pixels are the same ones on every platform's CI.
    gs_sanitizer_env(_lsan)
    set_tests_properties(gearstick_render PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIO_DRIVER=dummy;${_lsan}"
        TIMEOUT 180)

    # A track through a real file, not just a buffer: the format is what a
    # shared track travels in, so "it round-trips in memory" is not the claim
    # that matters.
    add_test(NAME gearstick_track_file
             COMMAND gearstick_cli track round_trip.gstrack)
    set_tests_properties(gearstick_track_file PROPERTIES TIMEOUT 60)

    # Every way a route can be broken is refused, and named.
    add_test(NAME gearstick_validate COMMAND gearstick_cli validate)

    # The importer, against a file this repository made. The corpus it is really
    # for belongs to somebody else and does not ship - see docs/ASSETS.md rule 1
    # - so a check that needed a download would be a check that does not run.
    add_test(NAME gearstick_import COMMAND gearstick_cli import)
    set_tests_properties(gearstick_validate PROPERTIES TIMEOUT 60)

    # The AI gets round, on every surface and in every machine, with no
    # recorded line and nothing tuned per track.
    add_test(NAME gearstick_ai COMMAND gearstick_cli ai)
    set_tests_properties(gearstick_ai PROPERTIES TIMEOUT 180)

    # The opponent is beatable by driving better and not merely by driving
    # longer: a quicker driver beats it and a more cautious one does not.
    add_test(NAME gearstick_pace COMMAND gearstick_cli pace)
    set_tests_properties(gearstick_pace PROPERTIES TIMEOUT 300)

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
    if(GEARSTICK_SERVER)
        install(TARGETS gearstick_server RUNTIME DESTINATION .)
    endif()
    install(FILES ${GS_DOCS} DESTINATION .)
else()
    install(TARGETS gearstick gearstick_cli RUNTIME DESTINATION .)
    # The server goes in the package too. docs/RELEASES.md lists it as something
    # a release contains, and a document describing a file nobody has is worse
    # than no document.
    if(GEARSTICK_SERVER)
        install(TARGETS gearstick_server RUNTIME DESTINATION .)
    endif()
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

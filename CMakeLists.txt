cmake_minimum_required(VERSION 3.28)

project(gearstick
    VERSION     0.1.0
    DESCRIPTION "Gearstick - a Racing Destruction Set-style construction racer in C23 + SDL3"
    # C is the project. CXX is here for exactly one reason: Dear ImGui, which
    # the editor's panels are built on and which is C++ underneath its C API.
    # Nothing of ours is written in it - see the gearstick_ui target below, and
    # docs/PROJECT_STATUS.md for why the exception was worth making.
    LANGUAGES   C CXX)

# ---------------------------------------------------------------------------
# **The C runtime goes inside the executable on Windows.**
#
# MSVC links the dynamic runtime by default, which means gearstick.exe needs
# VCRUNTIME140.dll and MSVCP140.dll from the Visual C++ Redistributable. On a
# machine that has them - anything with Visual Studio, or a build runner - that
# is invisible. On a machine that does not, the game does not start, and the zip
# has no way to fix it because a zip cannot install a redistributable.
#
# Static linking makes both Windows artifacts self-contained. It costs a few
# hundred kilobytes and removes the single most common reason a Windows game
# fails to start on somebody else's computer. Set before anything is added so
# that SDL, SQLite and libsodium are built the same way - two halves of one
# program disagreeing about the runtime is a link error at best.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
        "Static C runtime, so a release needs no redistributable")
endif()

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
    src/core/gs_parts.c
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
    src/net/gs_auth.c
    src/net/gs_noise.c
    src/net/gs_proto.c
    src/net/gs_carrier.c)

target_include_directories(gearstick_shell PUBLIC src)
target_link_libraries(gearstick_shell PUBLIC gearstick_sim gearstick_ui ${GS_SDL}
    ${GS_SDL_NET} gearstick_sodium)

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
    src/net/gs_auth.c
    src/net/gs_noise.c
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
    gearstick_sim gearstick_sqlite gearstick_sodium)
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
        src/net/gs_auth.c
        src/net/gs_store.c
        src/net/gs_verify.c)
    # The store and the password hasher, so a test can set up a profile the way
    # a sign-up would rather than through a protocol that has no message for it.
    target_link_libraries(gearstick_server_tests PRIVATE gearstick_shell
                          gearstick_sqlite gearstick_sodium)
    gs_configure(gearstick_server_tests)
    add_dependencies(gearstick_server_tests gearstick_server)

    # What the server remembers. Links no SDL: the store is SQLite and the
    # track format, and neither of those has ever needed a window.
    add_executable(gearstick_store_tests
        tests/test_store.c
        src/net/gs_auth.c
        src/net/gs_store.c)
    target_include_directories(gearstick_store_tests PRIVATE src)
    # gearstick_sim so the shipped library's tracks can be read back as tracks
    # rather than trusted as blobs; it links nothing, so this stays SDL-free.
    target_link_libraries(gearstick_store_tests PRIVATE gearstick_sqlite gearstick_sim
                                                     gearstick_sodium)
    target_compile_definitions(gearstick_store_tests PRIVATE
        GS_SOURCE_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    gs_configure(gearstick_store_tests)

    add_test(NAME gearstick_store COMMAND gearstick_store_tests)
    set_tests_properties(gearstick_store PROPERTIES TIMEOUT 120)

    # **Seven minutes, because this suite got much bigger.** Each test here
    # starts a real server process and talks to it over real sockets, and Phase
    # 14 added six more of them - sessions, a sealed mesh, ownership, sharing.
    # Ninety seconds on a developer's machine became more than three minutes on
    # a loaded macOS runner, and the old three-minute limit turned that into a
    # red tree that said "Timeout" and nothing about what was wrong.
    add_test(NAME gearstick_server COMMAND gearstick_server_tests)
    set_tests_properties(gearstick_server PROPERTIES TIMEOUT 420)

    add_test(NAME gearstick_wire COMMAND gearstick_wire_tests)
    set_tests_properties(gearstick_wire PROPERTIES TIMEOUT 120)

    # The tunnel. No sockets and no SDL: it turns bytes into bytes, so it is
    # tested against the framework's published vectors rather than by watching
    # two processes appear to talk.
    add_executable(gearstick_noise_tests tests/test_noise.c src/net/gs_noise.c)
    target_link_libraries(gearstick_noise_tests PRIVATE gearstick_sim gearstick_sodium)
    target_compile_definitions(gearstick_noise_tests PRIVATE
        GS_NOISE_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors/noise_ik_25519_chachapoly_blake2s.json")
    gs_configure(gearstick_noise_tests)

    add_test(NAME gearstick_noise COMMAND gearstick_noise_tests)
    set_tests_properties(gearstick_noise PROPERTIES TIMEOUT 120)

    # **A handshake against an implementation nobody here wrote.** The published
    # vectors say our bytes match a recording; they cannot say we can hold a
    # conversation, because both sides of a vector came from one implementation.
    # This is the evidence that does not rest on our own opinion of our own
    # code, and it is a separate program in a separate language for exactly
    # that reason.
    add_executable(gearstick_noise_peer tools/noise_peer.c src/net/gs_noise.c)
    target_link_libraries(gearstick_noise_peer PRIVATE gearstick_sim gearstick_sodium)
    gs_configure(gearstick_noise_peer)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND AND GEARSTICK_SERVER)
        # **A client written from the document, not from the code.** Every
        # constant and offset in tools/transport_client.py is quoted from
        # docs/TRANSPORT.md by section. If a byte offset in that document is
        # wrong, or the prologue, or the note about the protocol name being
        # hashed rather than padded, this fails to complete a handshake with a
        # real server - which is the only way a specification gets checked
        # against something other than its author's memory.
        add_test(NAME gearstick_transport_document
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/transport_client.py
                         --serve $<TARGET_FILE:gearstick_server>)
        set_tests_properties(gearstick_transport_document PROPERTIES
                             TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()

    # **The rule that lives in the main loop, checked on the real programs.**
    # A client that has signed in and is reading the records must not be pulled
    # onto the grid because a one-player server decided the race could start;
    # waiting in the lobby is how somebody says yes. That rule is in the
    # frontend, which links a window system, so no test executable can reach it
    # - it takes a real server and a real client, which is what this is.
    #
    # It checks both directions. "The menu did not start a race" would pass
    # just as well if nothing ever raced, so the lobby must still get in.
    if(Python3_FOUND AND GEARSTICK_SERVER AND TARGET gearstick)
        add_test(NAME gearstick_front_door
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/front_door_check.py
                         $<TARGET_FILE:gearstick_server>
                         $<TARGET_FILE:gearstick>)
        set_tests_properties(gearstick_front_door PROPERTIES
                             TIMEOUT 180 SKIP_RETURN_CODE 77)
    endif()

    # **The game plays itself, and says what it is showing.** Everything that
    # has gone wrong in front of a player went wrong after the green flag - a
    # camera pointed elsewhere, controls that did nothing, a race frozen two
    # seconds in - and none of it is reachable from a unit test or from the
    # front door check, which stops watching once a race has begun. The client
    # can drive itself now (--autodrive) and say what is on screen (--trace);
    # this races it on this machine and against a real server, and makes
    # assertions out of what it said.
    if(Python3_FOUND AND GEARSTICK_SERVER AND TARGET gearstick)
        add_test(NAME gearstick_plays
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/play_check.py
                         $<TARGET_FILE:gearstick_server>
                         $<TARGET_FILE:gearstick>
                         ${CMAKE_CURRENT_SOURCE_DIR}/assets)
        set_tests_properties(gearstick_plays PROPERTIES
                             TIMEOUT 240 SKIP_RETURN_CODE 77)
    endif()

    # **A server that stops serving because of its own output.** The dashboard
    # repaints four times a second; into a pipe nobody drains, that fills the
    # buffer and the next printf blocks - and a server blocked in a write
    # answers nothing. Windows pipes hold four kilobytes, about a second of it,
    # which is why the front door check above failed there and nowhere else.
    if(Python3_FOUND AND GEARSTICK_SERVER)
        add_test(NAME gearstick_server_output
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/server_output_check.py
                         $<TARGET_FILE:gearstick_server>)
        set_tests_properties(gearstick_server_output PROPERTIES
                             TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()

    if(Python3_FOUND)
        add_test(NAME gearstick_noise_interop
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/noise_interop.py
                         $<TARGET_FILE:gearstick_noise_peer>)
        # 77 is ctest's "skipped": the script says so when the noiseprotocol
        # package is absent, which is better than a green tick for a check that
        # did not run.
        set_tests_properties(gearstick_noise_interop PROPERTIES
                             TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()

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
# The primitives behind the transport
#
# X25519 and ChaCha20-Poly1305, from somebody else's audited library rather than
# from here. A handshake this project invented is the thing that fails a review,
# and deservedly; so is a curve this project implemented. BLAKE2s is the one
# exception and it is not an exception to the principle: libsodium ships BLAKE2b
# and the suite names BLAKE2s, and the simulation cannot link libsodium anyway.

include(cmake/Libsodium.cmake)
gs_add_libsodium()

# ---------------------------------------------------------------------------
# Fuzzing the parsers
#
# **Every byte the server acts on came from somebody who may be hostile**, and
# until this existed those parsers had never been fed anything but well-formed
# input. They are the part of this program most likely to contain a
# memory-safety bug, and they are reachable from the network by anybody.
#
# Off by default and clang-only, because libFuzzer is a clang feature. The
# targets link the simulation and the protocol and nothing else - no SDL, no
# window, no sockets - which is what makes them cheap enough to run on every
# push.

option(GEARSTICK_FUZZ "Build the libFuzzer parser targets (clang only)" OFF)

if(GEARSTICK_FUZZ)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "GEARSTICK_FUZZ needs clang: libFuzzer is a clang feature and "
            "${CMAKE_C_COMPILER_ID} does not have it. Configure with "
            "-DCMAKE_C_COMPILER=clang.")
    endif()

    # The seed writer is an ordinary program - it builds real messages with the
    # same code the game builds them with, so a seed cannot drift out of step
    # with the format it is a sample of. Not fuzzer-instrumented itself.
    add_executable(gearstick_fuzz_seeds
        tools/fuzz_seeds.c
        src/net/gs_proto.c
        src/net/gs_carrier.c
        src/net/gs_noise.c)
    target_link_libraries(gearstick_fuzz_seeds PRIVATE gearstick_sim gearstick_sodium)
    gs_configure(gearstick_fuzz_seeds)

    # ASan and UBSan alongside the fuzzer, because "it did not crash" is a much
    # weaker statement than "it did not read out of bounds or shift by 32", and
    # the second one is what these are for.
    set(GS_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined
                      -fno-omit-frame-pointer -g)

    foreach(_target proto carrier track replay noise)
        add_executable(gearstick_fuzz_${_target}
            tests/fuzz/fuzz_${_target}.c
            src/net/gs_proto.c
            src/net/gs_carrier.c
            src/net/gs_noise.c)
        target_link_libraries(gearstick_fuzz_${_target}
                              PRIVATE gearstick_sim gearstick_sodium)
        gs_configure(gearstick_fuzz_${_target})
        target_compile_options(gearstick_fuzz_${_target} PRIVATE ${GS_FUZZ_FLAGS})
        target_link_options(gearstick_fuzz_${_target} PRIVATE ${GS_FUZZ_FLAGS})
    endforeach()

    # A short run over the seed corpus, as a test. This is not a fuzzing
    # campaign - it is the regression half: the seeds go through every parser
    # under the sanitizers on every build, and CI runs the campaign properly
    # with a time budget. `-runs` rather than `-max_total_time` so the test
    # takes the same work on a slow machine as on a fast one and cannot go
    # flaky by being unlucky with a clock.
    if(GEARSTICK_TESTS)
        foreach(_target proto carrier track replay noise)
            add_test(NAME gearstick_fuzz_${_target}
                     COMMAND ${CMAKE_COMMAND}
                             -DSEEDS=$<TARGET_FILE:gearstick_fuzz_seeds>
                             -DFUZZER=$<TARGET_FILE:gearstick_fuzz_${_target}>
                             -DCORPUS=${_target}
                             -DDIR=${CMAKE_CURRENT_BINARY_DIR}/fuzz/${_target}
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFuzzer.cmake)
            set_tests_properties(gearstick_fuzz_${_target} PROPERTIES TIMEOUT 300)
        endforeach()
    endif()
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
    # **Both, and the zip is not an afterthought.** On Windows an application
    # people actually install is an installer; somebody who wants a folder to
    # unpack still gets one, because a zip needs no administrator and leaves
    # nothing behind by construction.
    set(CPACK_GENERATOR "ZIP;WIX")

    # **The upgrade GUID is fixed for ever.** It is how Windows knows that
    # version 0.2 is the same product as version 0.1 rather than a second copy
    # to install alongside it. Generating one per build would leave somebody
    # with every version they ever installed still on the machine, and there is
    # no way to fix that after the fact for the people it happened to.
    set(CPACK_WIX_UPGRADE_GUID "6F3B8C21-9D4E-4A17-B0C5-2E7A1D8F4B93")
    set(CPACK_WIX_PRODUCT_GUID "*")          # a new one per version, as WiX wants

    set(CPACK_WIX_PROGRAM_MENU_FOLDER "Gearstick")
    set(CPACK_WIX_UPGRADE_VERSION_MAX "255.0.0")
    set(CPACK_WIX_ROOT_FEATURE_TITLE  "Gearstick")
    set(CPACK_WIX_ROOT_FEATURE_DESCRIPTION
        "${PROJECT_DESCRIPTION}")

    # The Start Menu entry. Only the game: the headless driver and the server
    # are command-line programs, and a shortcut that opens a console window and
    # closes it again is worse than no shortcut.
    set(CPACK_PACKAGE_EXECUTABLES "gearstick;Gearstick")
    set(CPACK_CREATE_DESKTOP_LINKS "gearstick")

    # **An MSI's product version must be `x.x.x.x` and nothing else.**
    #
    # Windows Installer will not take `0.1.0-beta1`: it wants four integers,
    # and it uses that number - not the file name - to decide whether an
    # install is an upgrade. So the version the package *calls itself* and the
    # version inside the MSI have to be allowed to differ.
    #
    # The numeric part is taken as it stands, and a pre-release ending in
    # digits contributes the fourth field: `0.1.0-beta1` becomes `0.1.0.1`.
    # Without that last step every beta of 0.1.0 would carry the same product
    # version, and installing beta2 over beta1 would not be seen as an upgrade
    # at all - which is the kind of thing nobody notices until somebody is
    # running two of them.
    string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" GS_MSI_NUMBER "${GEARSTICK_RELEASE}")
    if(NOT GS_MSI_NUMBER)
        set(GS_MSI_NUMBER "0.0.0")
    endif()
    string(REGEX MATCH "[0-9]+$" GS_MSI_PRE "${GEARSTICK_RELEASE}")
    if(NOT GEARSTICK_RELEASE STREQUAL GS_MSI_NUMBER AND GS_MSI_PRE)
        set(GS_MSI_NUMBER "${GS_MSI_NUMBER}.${GS_MSI_PRE}")
    endif()
    set(CPACK_GS_MSI_VERSION "${GS_MSI_NUMBER}")
    message(STATUS "gearstick: the MSI's product version is ${GS_MSI_NUMBER}")

    # CPack reads this once per generator, which is the only place the version
    # can be changed for the MSI without changing what every other package
    # calls itself.
    set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPackProject.cmake")

    # **WiX refuses a licence file it cannot recognise by extension**, and this
    # project's is called `LICENSE` with no extension at all - which CPack's WIX
    # generator rejects outright rather than guessing. So a copy with a `.txt`
    # on it is made in the build tree and handed over; the file in the
    # repository keeps the name every tool expects it to have.
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                   "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ;DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Gearstick ${PROJECT_VERSION}")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)

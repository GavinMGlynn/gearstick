# SQLite for the server, and for nothing else.
#
# **The easiest thing that works the same on all three platforms.** SQLite ships
# an "amalgamation": the whole library as one C file. Compiling that needs no
# package manager, no pkg-config, no Tcl and no submodule - it is a source file,
# and every platform this project supports can compile a source file.
#
# It is downloaded rather than committed, because `docs/ASSETS.md` rule 1 says
# nothing third-party is redistributed by this repository. Rule 3 covers exactly
# this case: a non-git source is pinned by URL and SHA-256, which is what the
# two lines below are. The download lands in the build tree, never in the repo.
#
# A system SQLite is used if there is one, because a developer who already has
# it should not wait for a download - but nothing depends on there being one.
#
# **Only `gearstick_server` links this.** The game, the simulation and the
# headless driver are untouched: the client's store is the same versioned flat
# format it has always been, and `gearstick_cli` still links nothing but libc.

set(GS_SQLITE_VERSION "3.53.1")
set(GS_SQLITE_ZIP     "sqlite-amalgamation-3530100")
set(GS_SQLITE_URL     "https://sqlite.org/2026/${GS_SQLITE_ZIP}.zip")
set(GS_SQLITE_SHA256  "36ad6e7f38540a3b21a2ac36340833f0a9e426bc1c752751c3ba669466827eae")

function(gs_add_sqlite)
    if(TARGET gearstick_sqlite)
        return()
    endif()

    # A system copy, if there is one.
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND AND NOT GEARSTICK_SQLITE_AMALGAMATION)
        add_library(gearstick_sqlite INTERFACE)
        target_link_libraries(gearstick_sqlite INTERFACE SQLite::SQLite3)
        message(STATUS "gearstick: SQLite ${SQLite3_VERSION} from the system")
        return()
    endif()

    set(_dir "${CMAKE_BINARY_DIR}/_sqlite")
    set(_src "${_dir}/${GS_SQLITE_ZIP}/sqlite3.c")

    if(NOT EXISTS "${_src}")
        message(STATUS "gearstick: fetching SQLite ${GS_SQLITE_VERSION}")
        file(DOWNLOAD "${GS_SQLITE_URL}" "${_dir}/sqlite.zip"
             EXPECTED_HASH SHA256=${GS_SQLITE_SHA256}
             TLS_VERIFY ON
             STATUS _status)
        list(GET _status 0 _code)
        if(NOT _code EQUAL 0)
            list(GET _status 1 _why)
            message(FATAL_ERROR
                "could not fetch SQLite: ${_why}\n"
                "  ${GS_SQLITE_URL}\n"
                "The server needs it. Either give this machine a network for one\n"
                "configure, install SQLite and let find_package see it, or turn\n"
                "the server off with -DGEARSTICK_SERVER=OFF.")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_dir}/sqlite.zip" DESTINATION "${_dir}")
    endif()

    add_library(gearstick_sqlite STATIC "${_src}")
    target_include_directories(gearstick_sqlite SYSTEM PUBLIC
        "${_dir}/${GS_SQLITE_ZIP}")

    # Third-party code is compiled on its own terms. The project's warning set
    # is for the project's code - see CLAUDE.md, which says the same thing about
    # everything under ext/.
    if(MSVC)
        target_compile_options(gearstick_sqlite PRIVATE /w)
    else()
        target_compile_options(gearstick_sqlite PRIVATE -w)
    endif()

    # What the server actually needs, and nothing else. Every one of these
    # removes code rather than adding it: no loadable extensions is one less
    # way for a server to be talked into running something, and the rest are
    # features a records table has no use for.
    target_compile_definitions(gearstick_sqlite PUBLIC
        SQLITE_OMIT_LOAD_EXTENSION=1
        SQLITE_OMIT_DEPRECATED=1
        SQLITE_DQS=0                    # a bare string is never an identifier
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_THREADSAFE=1
        SQLITE_ENABLE_COLUMN_METADATA=1)

    if(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(gearstick_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    message(STATUS "gearstick: SQLite ${GS_SQLITE_VERSION} from the amalgamation")
endfunction()

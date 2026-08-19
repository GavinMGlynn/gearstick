# Layering.cmake - the two rules that make the simulation what it is, checked
# at configure time rather than remembered.
#
# **Rule 1: `src/core/` is the simulation and does not know it is being looked
# at.** No renderer, texture, window, event, gamepad or audio stream - and no
# include of gfx/, ui/, platform/, debug/ or a frontend. That is what lets the
# editor's background ghost re-race a track while you edit it, lets the headless
# CLI sweep a track across gravity values in CI, and lets a unit test run ten
# thousand ticks with no video driver.
#
# Unlike the sibling projects, core here may not use SDL *at all* - not even for
# memory or logging. The sim is a standalone C library so that the headless
# driver, the test harness and any future rollback host link the simulation
# without linking a window system. `#include <SDL3/...>` in core is a violation.
#
# **Rule 2: no floating point in `src/core/`.** The simulation is Q16.16 fixed
# point with int64 intermediates. Determinism is not a nice property here, it is
# the feature every other feature is built on - input-log replays, ghost times
# that mean something across machines, content-hashed tracks that aggregate
# ghosts, and rollback netcode that resimulates without diverging. One `float`
# in the physics step and x87 excess precision, FMA contraction or a different
# libm makes two machines disagree by a bit and then by a car length.
#
# Both are easy to break by accident with one convenient #include, and the
# damage shows up much later and somewhere else, so neither is left to review.

function(gs_check_layering)
    file(GLOB_RECURSE _core_sources
         "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h")

    set(_violations "")
    foreach(_f ${_core_sources})
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_f}")

        # Reaching into a presentation layer.
        file(STRINGS "${_f}" _bad
             REGEX "^[ \t]*#include[ \t]+\"(gfx|ui|audio|debug|frontend|platform)/")
        foreach(_line ${_bad})
            list(APPEND _violations "  ${_rel}: ${_line}")
        endforeach()

        # SDL at all - see Rule 1. The sim links no window system.
        file(STRINGS "${_f}" _sdlbad REGEX "^[ \t]*#include[ \t]+<SDL3?[/_]")
        foreach(_line ${_sdlbad})
            list(APPEND _violations "  ${_rel}: ${_line}")
        endforeach()

        # Floating point - see Rule 2. Comment lines are skipped so the rule can
        # be *explained* in core without tripping over its own name; a line of
        # code that merely ends in a comment is still checked.
        file(STRINGS "${_f}" _lines REGEX "(float|double)")
        foreach(_line ${_lines})
            if(_line MATCHES "^[ \t]*(//|\\*|/\\*)")
                continue()
            endif()
            # `-Wdouble-promotion` and suchlike name the type in a pragma or an
            # attribute; those are not arithmetic.
            if(_line MATCHES "^[ \t]*#[ \t]*pragma")
                continue()
            endif()
            if(_line MATCHES "(^|[^A-Za-z0-9_])(float|double)([^A-Za-z0-9_]|$)")
                list(APPEND _violations "  ${_rel}: ${_line}")
            endif()
        endforeach()
    endforeach()

    if(_violations)
        string(REPLACE ";" "\n" _report "${_violations}")
        message(FATAL_ERROR
            "src/core/ has broken the simulation boundary:\n"
            "${_report}\n"
            "core/ must not depend on gfx, ui, audio, debug, platform or the "
            "frontends, must not include SDL, and must not use floating point. "
            "See cmake/Layering.cmake for why each of those costs more than it "
            "looks like it does.")
    endif()
    message(STATUS "gearstick: src/core/ is SDL-free, float-free and self-contained")
endfunction()

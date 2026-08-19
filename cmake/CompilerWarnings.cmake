# CompilerWarnings.cmake - the warning set, applied to first-party targets only.
#
# Vendored code in ext/ is deliberately untouched: SDL builds clean under its
# own settings and would not survive -Wconversion, and warnings we cannot fix
# are warnings people learn to scroll past.

function(gs_configure target)
    if(NOT GS_HAVE_NULLPTR)
        target_compile_definitions(${target} PRIVATE GS_NO_NULLPTR)
    endif()
    target_compile_options(${target} PRIVATE ${GS_C_FLAGS})

    if(MSVC)
        # MSVC deprecates fopen, snprintf and friends in favour of its own _s
        # variants, and /WX turns that opinion into a build failure. Those
        # functions are standard C; the _s ones are Annex K, which most
        # implementations do not ship. Taking Microsoft's advice would mean
        # writing a second I/O path for one compiler, so we decline it here
        # rather than sprinkling pragmas through the frontends.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
        if(GEARSTICK_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        # -Wunused-parameter stays on deliberately. MSVC's /W4 includes C4100
        # and /WX makes it fatal, so a dead parameter that compiles clean on
        # every developer machine fails only in CI, on the one platform nobody
        # builds locally. The warning sets have to agree, or the strictest one
        # is really a remote lint nobody can run.
        #
        # -Wconversion and -Wsign-conversion are the load-bearing pair here:
        # this is fixed-point code, full of int32/int64 mixing, and a silent
        # narrowing in the physics step is a desync rather than a crash.
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
            -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes
            -Wunused-parameter -Wdouble-promotion)
        if(GEARSTICK_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(GEARSTICK_ASAN AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# The suppressions LeakSanitizer needs to be useful, as an environment setting.
#
# Without it a sanitized run of anything that opens an audio device reports two
# allocations inside SDL's PulseAudio backend, and a report that is noisy on a
# clean tree is a report nobody reads. See cmake/lsan.supp for what is in it and
# why each entry is somebody else's leak.
function(gs_sanitizer_env out)
    if(GEARSTICK_ASAN AND NOT MSVC)
        set(${out} "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/cmake/lsan.supp"
            PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# Platform.cmake - the supported set, enforced at configure time.
#
# Gearstick targets exactly three platforms, all 64-bit:
#
#   Linux x86_64    both RHEL-family and Debian-family
#   Windows x64     MSVC or clang-cl
#   macOS arm64     Apple Silicon
#
# Enforced rather than assumed. A 32-bit build would compile and then diverge:
# the simulation is Q16.16 fixed point with 64-bit intermediates, and a build
# where `long long` arithmetic is emulated differently is a build whose replays
# do not match everyone else's. Determinism is the product here, so an
# unsupported target is refused with a message that says so.

if(CMAKE_SIZEOF_VOID_P LESS 8)
    message(FATAL_ERROR
        "gearstick is 64-bit only.\n"
        "  found:  ${CMAKE_SIZEOF_VOID_P}-byte pointers on ${CMAKE_SYSTEM_PROCESSOR}\n"
        "  needed: a 64-bit toolchain")
endif()

set(GS_PLATFORM "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(GS_PLATFORM "linux-x86_64")
elseif(WIN32)
    set(GS_PLATFORM "windows-x64")
elseif(APPLE)
    set(GS_PLATFORM "macos-${CMAKE_SYSTEM_PROCESSOR}")
else()
    message(WARNING
        "gearstick has no CI for ${CMAKE_SYSTEM_NAME}; it may build, but "
        "nothing verifies that it works. Supported: Linux x86_64, Windows x64, "
        "macOS arm64.")
    set(GS_PLATFORM "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()

# ---------------------------------------------------------------------------
# Compiler gate - C23 needs a recent toolchain.
#   GCC 14+   Clang 19+   AppleClang 16+ (Xcode 16)   MSVC 19.39+ (VS 17.9)
# RHEL 9 and Ubuntu 22.04 both default to GCC 11, which is too old; install
# gcc-toolset-14 (RHEL/Rocky) or gcc-14 from the toolchain PPA (Debian/Ubuntu).
# ---------------------------------------------------------------------------
set(_gs_min "")
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(_gs_min 14)
elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set(_gs_min 19)
elseif(CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    set(_gs_min 16)
elseif(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(_gs_min 19.39)
endif()

if(_gs_min AND CMAKE_C_COMPILER_VERSION VERSION_LESS _gs_min)
    message(FATAL_ERROR
        "gearstick needs C23 support.\n"
        "  found:    ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\n"
        "  required: ${CMAKE_C_COMPILER_ID} ${_gs_min} or newer\n"
        "Point CMake at a newer compiler, e.g.\n"
        "  cmake -B build -DCMAKE_C_COMPILER=gcc-14")
endif()

message(STATUS "gearstick: ${GS_PLATFORM}, ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (C23)")

# MSVC has no /std:c23 yet; its newest C features sit behind /std:clatest, and
# /Zc:__STDC__ makes it report the standard version macros honestly. MSVC's C23
# is partial - if a gap bites, the documented fallback for Windows is clang-cl,
# which the windows-clang preset selects.
set(GS_C_FLAGS "")
if(MSVC)
    set(GS_C_FLAGS /std:clatest /Zc:__STDC__)
endif()

# C23 support is uneven: a compiler can take the flag and still not implement a
# given feature. Probe for `nullptr` rather than guessing from version numbers,
# using the same flags the real build gets, so a compiler that gains it later
# simply stops needing the shim.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "${GS_C_FLAGS}")
list(JOIN CMAKE_REQUIRED_FLAGS " " CMAKE_REQUIRED_FLAGS)
check_c_source_compiles(
    "int main(void) { void *p = nullptr; return p != (void *)0; }"
    GS_HAVE_NULLPTR)
unset(CMAKE_REQUIRED_FLAGS)

if(GS_HAVE_NULLPTR)
    message(STATUS "gearstick: C23 nullptr - native")
else()
    message(STATUS "gearstick: C23 nullptr - not implemented, using a compatibility macro")
endif()

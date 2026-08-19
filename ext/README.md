# Third-party dependencies

Everything here is a pinned git submodule. Nothing in this directory is our
code, nothing here is modified in place, and nothing here is redistributed by
this repository — a clone gets URLs and commit SHAs, not sources.

## What is here

| Submodule | Upstream | Pinned at | Role | Licence |
| --- | --- | --- | --- | --- |
| `sdl` | libsdl-org/SDL | `release-3.4.14` | **Linked by the shell only.** Window, renderer, input, audio, filesystem, timing | Zlib |
| `imgui` | ocornut/imgui | `v1.92.9b` | **The editor's UI.** Not built yet — see below | MIT |
| `dear_bindings` | dearimgui/dear_bindings | `v0.21` | **Build-time only.** Generates the C API for the above; never linked | MIT |
| `sdl_net` | libsdl-org/SDL_net | `release-3.2.0` | **Linked by the shell only.** The datagram socket the rollback netcode sends over | Zlib |

`sdl_net` is a separate submodule rather than part of `sdl` because networking
is a separate library from SDL, and it is a submodule rather than a system
package because no distribution ships the SDL3 version yet. It is used for
datagrams and nothing else: the rollback netcode wants a socket that is allowed
to drop and reorder, and every comfort above that is a round trip.

Nothing links `imgui` yet. The first Phase 4 items — the track file format,
identity by content, and the undo model — are pure C and needed whichever way
the UI goes, so the submodules are pinned now and wired into the build when
there are widgets to draw.

**Dear ImGui is C++.** `dear_bindings` generates a C API over it, which is what
makes it usable here, but the implementation underneath is still C++: adopting
it means `LANGUAGES C CXX` and the C++ standard library linked on all three
platforms. That is a deliberate exception to this project's preference for no
dependency over a small one, taken because palettes, property panels, validation
output and live tuning dials are the part of an editor that C is worst at and
the part that kills projects shaped like this one. The simulation is untouched
by it: `src/core/` still links nothing, and `gearstick_cli` still links only the
simulation.

`dear_bindings` v0.21 is the release that pairs with Dear ImGui v1.92.9b, and
both the SDL3 and the SDL_Renderer3 backends are current as of August 2026. That
pairing is not optional — the generator and the header it reads have to match,
so the two pins move together or not at all.

**Building needs `ext/imgui` but not `ext/dear_bindings`**, because the
generated C API is committed under `src/ui/dcimgui/`. Only regenerating needs
the generator, and `.github/workflows/tables.yml` is the one job that checks it
out — which is also the job that proves the committed bindings really are what
the pinned inputs produce.

## What the build actually needs

```sh
git submodule update --init --depth 1 ext/sdl
cmake --preset linux-release && cmake --build --preset linux-release
ctest --preset linux-release
```

Or, to build against an SDL3 the system already has:

```sh
cmake -B build -G Ninja -DGEARSTICK_USE_SYSTEM_SDL=ON
```

`CMakeLists.txt` fails with an actionable message if `ext/sdl` is missing rather
than with a wall of CMake.

## What is deliberately not here

**No image library.** `SDL_image` was checked out for this and then removed
before it was ever built, because nothing decodes an image: the terrain is
emitted as shaded geometry rather than assembled from a tile atlas, precisely so
that arbitrary elevation joins stitch by construction. Until there are vehicle
sprites to decode, an image decoder is a dependency on three platforms for a
file type this game does not ship. It comes back with the art pipeline in
Phase 10, and not before.

**No audio library.** SDL3 loads and converts WAVs by itself, and the engine
note is going to be generated rather than sampled, so mixing is a few hundred
lines rather than a dependency.

**No maths library in the simulation.** `gearstick_sim` links nothing at all —
not SDL, not libm. The tests link libm to check the fixed-point trigonometry
against double precision, which is the only place a maths library appears.

## Building SDL needs system development packages

SDL is vendored as source, so the *system* still has to carry what SDL links
against. The authoritative list is `docs/README-linux.md` inside the submodule
at the pinned tag; `.github/workflows/ci.yml` installs it on both Linux
families and is the copy that is actually tested.

Two of them surprise people, because SDL does not treat them as optional — it
calls `SDL_missing_dependency()` and fails the configure, naming a `-D` flag
rather than a package:

- **XTEST** — `libxtst-dev` / `libXtst-devel`
- **XScrnSaver** — `libxss-dev` / `libXScrnSaver-devel`

On RHEL-family systems `libXScrnSaver-devel` is **in EPEL and nowhere else**, so
a stock RHEL or Rocky box needs `epel-release` installed before it can build
this at all. `ninja-build` and `libdecor-devel` come from CRB. Neither is a CI
quirk; both apply to anybody building from source.

If you would rather not have them, `-DSDL_X11_XTEST=OFF` and
`-DSDL_X11_XSCRNSAVER=OFF` are the documented escapes. Gearstick does not take
them: a racing game genuinely wants to stop the screensaver coming on mid-race.

## Why submodules rather than FetchContent

A clone records the exact commit it builds against, and an offline build works.
`ext/sdl` is a shallow checkout of one release tag; nothing here needs history.

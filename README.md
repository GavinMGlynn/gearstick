# gearstick

A construction-set racer in C23 and SDL3 — *Racing Destruction Set*, 1985,
continued rather than remade.

> **Status: early.** The simulation runs and is deterministic to the bit —
> the same race lands on the same state hash on Ubuntu, Rocky, macOS arm64 and
> Windows, across three compilers, and CI fails if that ever stops being true.
> A car drives, jumps, slides on ice and folds on a bad landing; gravity is
> paintable per tile; a race records and re-races exactly. There is a window
> with a split screen, an isometric renderer and a hard-coded demo track.
>
> **There is no editor, no lap, no opponent, no sound and no way to win.** The
> editor is roughly half this project and it has not been started.
> [`docs/PROJECT_STATUS.md`](docs/PROJECT_STATUS.md) is the single source of
> truth for what works, with the gaps named plainly. Nothing here claims more
> than it has earned.

## The idea

*Racing Destruction Set* handed you fourteen gravity settings, three surfaces, a
two-car track editor and no progression whatsoever, then got out of the way.
Its argument was: **everything is a dial, nothing is locked, the model is simple
enough to predict and therefore exploit, and the chaos is the reward.**

Gearstick continues that argument with the things 1985 could not afford:

- **Gravity is a brush, not a setting.** Every tile carries its own multiplier,
  so a low-gravity pocket at the top of a jump or a Jupiter zone through a
  banked turn is something you paint.
- **Determinism as a product feature.** A race is reproducible from its inputs,
  which is what makes ghosts, replay sharing, a track analyser and rollback
  netcode possible at all.
- **A track leaves the room.** The 50-track floppy was a media limitation, not a
  design choice.

What it deliberately does *not* add — slip curves, progression, a chase camera —
and why, is in [`docs/FEATURES.md`](docs/FEATURES.md). Where the art, sound and
tracks come from, and how each is pinned, is in
[`docs/ASSETS.md`](docs/ASSETS.md).

## Building

Needs CMake 3.28+, Ninja, and a C23 compiler: GCC 14+, Clang 19+, AppleClang 16+
or MSVC 19.39+. Targets Linux x86_64, Windows x64 and macOS arm64, all 64-bit.

```sh
git submodule update --init --depth 1 ext/sdl
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
./build/linux-release/gearstick
```

Presets exist for `linux`, `macos`, `windows` (MSVC) and `windows-clang`, each in
`-debug` and `-release`. To build against an SDL3 the system already has, add
`-DGEARSTICK_USE_SYSTEM_SDL=ON`.

## Playing, such as it is

Arrows drive car one, WASD car two; pads work, one per car. `G` toggles the
painted-gravity overlay, `R` restarts, `Esc` quits.

```sh
gearstick --shot frame.bmp --shot-at 420 --overlay   # write one frame and exit
```

## The headless driver

`gearstick_cli` links the simulation **and nothing else** — no SDL, no window, no
audio device. If it stops linking, the simulation has grown a dependency on
being looked at.

```sh
gearstick_cli selftest --verify   # re-race the fixed scenario, check the hash
gearstick_cli vehicles            # the roster and its numbers
gearstick_cli gravity             # the presets
```

That `--verify` is the tripwire: it re-races a fixed input log and compares one
state hash. If the number moves, every ghost time and every shared replay in
existence just became wrong.

## Layout

```
src/core/       the simulation - links nothing, integers only, no pointers in state
src/gfx/        isometric projection, terrain geometry
src/platform/   asset paths, keyboard and gamepad
src/frontend/   one main.c per executable: game/ and cli/
cmake/          platform gate, warning set, layer and float checks
tools/          the trig table baker
ext/            one pinned submodule - see ext/README.md
docs/           FEATURES.md, COMPLETION_PLAN.md, PROJECT_STATUS.md, ASSETS.md
```

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).

The original *Racing Destruction Set* is EA's, 1985, by Rick Koenig, Connie
Goldman and Dave Warhol. None of its tracks, art or code are here, and none will
be.

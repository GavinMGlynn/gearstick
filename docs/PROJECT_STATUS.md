# Project status

**What this file is.** The single source of truth for what actually works, and
the place detail belongs. `FEATURES.md` says what the game should be;
`COMPLETION_PLAN.md` says what order it gets built in and how each item is
verified; this file says what is true *today*, with the gaps named first.

**Never describe a partial module as working.** "Working" means 100% of what it
claims. Anything less is reported with the missing part named *first* — "the
terrain samples continuously and nothing has driven a lap on it", never "the
terrain works".

---

## The honest summary, 2026-08-19

**There is still no game.** No lap counting, no finish, no opponent, no
collision between cars, no hazard, no sound, no menu, and no way to win or
lose. What there is now is a construction set and a simulation to drive in it.

**The editor works.** Tab switches between building and driving with nothing
loaded in between. You can raise and lower ground, paint surface and gravity,
place a route of gates, undo any of it, save it, reload it and drive it — with
a mouse or with a pad alone. A ghost re-races the design in the background as
you change it.

**What there is, is a simulation that works and can prove it.** A car drives,
slides, climbs, launches off any shape that falls away fast enough, flies a
ballistic arc, and takes damage proportional to how badly it met the ground.
Gravity is a per-tile field you can paint. A race records as one byte per car
per tick and re-races to the same state, to the bit, on every compiler and
optimisation level tried. There is a window, a split screen and an isometric
renderer that draws the ground as shaded geometry, with a shadow under each car.

Twenty-six tests and a golden-replay tripwire, all green, on four platforms.
CI builds and tests on Ubuntu, Rocky, Windows and macOS arm64, and all four land
on the same state hash — so "the simulation is deterministic across machines" is
now a measured fact rather than a design intention. Packages build on all three
target platforms and are checked by unpacking them somewhere else and running
what is inside.

---

## What works

### The simulation — `src/core/`, linked into `gearstick_sim`

**Links nothing at all.** Not SDL, not libm. `ldd gearstick_cli` lists libc and
the dynamic loader, and that is the whole dependency list of the headless
driver. This is enforced at configure time by `cmake/Layering.cmake`, verified
by deliberately breaking it: an `#include <SDL3/SDL.h>` and a stray `double`
each fail the configure with the file and the offending line named.

- **Q16.16 fixed-point arithmetic** with `int64` intermediates, integer square
  root, and vector length computed in Q32.32 so the square cannot overflow
  before the root is taken.
- **Trigonometry from a generated, committed table.** A quarter turn of sine in
  1024 steps with the low bits interpolated, and a 16-entry CORDIC arctangent
  table. Agrees with double precision to better than 1e-4 across a full turn;
  `gs_atan2` inverts it to under a fifth of a degree. `tools/make_tables.py`
  reproduces `src/core/gs_tables.h` byte for byte.
- **Angles are a full turn in 65536 steps in a `uint16_t`**, so wrapping is the
  type's job. There is no modulus anywhere in the physics.
- **Terrain as per-corner heights** on a lattice with the maximum stride, so
  resizing a track in the editor will never have to move the data. Height at any
  point is a bilinear sample; the gradient is the average of the tile's two
  edges along each axis, which is the plane of best fit through four corners
  that need not be coplanar. Taking one edge alone makes a twisted tile read as
  flat from one side and steep from the other.
- **Three surfaces** — pavement, dirt, ice — each a grip limit expressed as a
  multiple of gravity, a rolling resistance and a fraction of the engine that
  reaches the ground.
- **Per-tile gravity**, a byte per tile where 64 is 1×, spanning nothing at all
  to just under 4×. Sampled every tick at the car's position, never cached.
- **Tracks identified by content hash**, FNV-1a over the used region only and
  byte-explicit, so identity does not depend on `GS_TRACK_MAX` or on
  endianness. Two tracks built independently to the same design hash equal; a
  one-tile edit changes it; undoing the edit restores it.
- **Six vehicles** as a table of blunt trade-offs — power, brake, top speed, tyre
  grip, steering authority, drag, toughness. `gearstick_cli vehicles` prints it.
- **The physics step, fixed at 120 Hz.** Steering that loses authority with
  speed; drive force that falls to nothing at the vehicle's top speed; a grip
  limit that scrubs sideways velocity at whatever the tyres and surface will
  bear; rolling resistance and air drag; and the slope pulling the car downhill.
- **Airborne flight.** There is no ramp tile type. A car leaves the ground when
  the ground falls away faster than gravity can hold it to it, which is what
  makes any shape a player builds a jump. No engine and no steering in the air,
  deliberately: the arc being non-negotiable is what makes the take-off decision
  matter.
- **Landing damage from mismatch, not from speed.** What hurts is the difference
  between how fast the car is coming down and how fast the ground is falling
  away beneath it, so a downhill landing barely registers and the same jump onto
  the flat folds the sprint car. Below a threshold a landing is free; above it,
  damage and speed loss climb with the excess.
- **The world state has no pointers**, so a snapshot is a `memcpy`. Tested by
  taking one, running three hundred ticks, restoring, re-running the same
  inputs, and landing on the same hash.
- **Replays as inputs, not positions.** One byte per car per tick, a header
  carrying the conditions and the track's content hash, and an explicit
  little-endian wire format. 900 ticks of two cars is 3,644 bytes. A replay
  against the wrong track is refused rather than quietly re-raced somewhere
  else.

### Determinism — measured, not asserted

The selftest race — 900 ticks, two cars, a ramp, an ice field and a painted
low-gravity pocket — ends on **the same state hash everywhere it has been run**:
locally under GCC at `-O0`, `-O1`, `-O2`, `-O3` and `-Os` and under Clang,

and, in CI, on **Ubuntu 24.04 (GCC 14), Rocky Linux 10 (GCC), macOS arm64
(AppleClang 17) and Windows x64 (MSVC)**. Four platforms, three compilers, two
architectures, one number. That is the entire argument for the integer
discipline, and it is now evidence rather than reasoning.

### The golden hash has moved twice, both on purpose

It is the tripwire that says the physics changed, so every time it moves the
reason is recorded here rather than quietly absorbed:

1. **Gates joined a track's identity.** Only the *track* hash moved; the world
   hash did not, because the simulation does not read gates. That is exactly
   what keeping the two numbers apart is for.
2. **The grip circle.** Engine force is now capped by the traction available,
   so tyres matter off the line and not only in corners, and low gravity takes
   your acceleration away along with your weight. Every replay and ghost time
   recorded before this is invalid. It was made because the roster sweep found
   the alternative: with traction limiting cornering alone, nothing but top
   speed decided a race and one vehicle won all seven conditions.

### The frame clock — `src/core/gs_clock.c`

The accumulator that turns "however long that frame took" into "how many fixed
steps are owed" lives in the simulation rather than beside the window, because
its correctness is a *simulation* property: get it wrong and the same input log
produces different races on a fast machine and a slow one. It takes nanoseconds
as an argument rather than asking SDL for the time, which is what makes it
testable at all.

One second of wall clock chopped into 30, 60, 144 and 240 frames each deliver
exactly 120 ticks. The 240 case is the one that bites — every frame is shorter
than a tick, so the leftover in the accumulator is the only thing that makes the
simulation advance, and a clock that discards its remainder scores zero there
while looking perfect at 30.

### The editor — `src/ui/`, inside `gearstick`

- **It is a mode of the game, not a second program**, because the loop between
  changing something and feeling it is the point, and a separate binary puts a
  reload in the middle of it. Tab drops a car where the pointer was, on the very
  track object being edited. Coming back returns the camera to the part of the
  track being *built*, not to wherever the car stopped.
- **Brushes**: raise, lower, surface, gravity, and gate. One application moves
  the ground by exactly the step shown in the panel. The brush is round rather
  than square, because a square one leaves corners in the terrain nobody drew.
- **Undo and redo** over any of it, a drag counting as one action however many
  tiles it touched. Placing a gate is the exception and is recorded as a tail.
- **A route of gates**, each directional and finite: reversing over the finish
  is not a lap, and a gate can be missed. Gate zero is the start.
- **Validation, shown continuously** rather than on a button — five problems,
  each naming the gate at fault.
- **The live ghost** re-races the track while you edit it, noticing that the
  track changed by its own content hash rather than by being told.
- **A pad drives all of it**, panel included, through ImGui's gamepad
  navigation.

### The renderer — `src/gfx/`, `src/platform/`, `gearstick`

- **2:1 isometric projection.** One tile is 64 px across, 32 deep, and one tile
  of height is 32 px, so elevation reads at the same scale as distance.
- **Terrain as two shaded triangles per tile**, tinted by surface and lit by
  slope, emitted with `SDL_RenderGeometry`. Sampling exactly on a corner lands
  on that corner — the bilinear weights are 0 and 1 — so adjacent tiles
  necessarily agree about the vertex they share. That is what "stitches by
  construction" means, and it is why there is no seam to hide.
- **The painted-gravity overlay**, violet where the ground pulls less and amber
  where it pulls more. A brush you cannot see is a dial, so this is not a debug
  view.
- **Cars as boxes with a blob shadow** on the ground beneath them. Crude, and
  the gap between car and shadow is the only cue that says how high a car is —
  the same thing that sold it on a C64.
- **Cars are drawn about 1.3 tiles long, which is not their metric size.** An
  honest 2.7 m car is two thirds of a tile and reads as a speck against the
  ground. The original's were never honest either: chunky relative to the view
  is what makes a two-car collision legible, and legibility is the whole
  argument for this camera. Nothing in `src/core/` knows these numbers today;
  when collision arrives it has to use them rather than the metric truth, or a
  car is hit by something the player cannot see.
- **Split screen**, two views side by side, one per car.
- **A fixed 120 Hz accumulator with render interpolation**, including
  short-way-round heading interpolation so a car crossing north does not spin on
  the spot for a frame.
- **`--shot FILE --shot-at TICK`** writes one frame and exits, counting ticks
  rather than reading the clock, so the captured frame is the same frame on
  every machine.
- **Keyboard and gamepad**, one pad per car, pads arriving and leaving handled.

### The build, CI and packaging

Configures, builds and tests green on Linux x86_64 with GCC 14.3.1, both against
the vendored `ext/sdl` at `release-3.4.14` and against a system SDL3 3.2.4.

CI runs four jobs — Ubuntu 24.04, Rocky Linux 10, macOS 15 arm64, Windows x64 —
each building, testing and rendering a frame headless. The `package` workflow
produces a tarball, a zip and a disk image, then **unpacks each into a different
directory and runs what is inside**: the headless driver re-races the golden
replay, and the game draws a frame, which is the step that proves the packaged
layout is one the asset probe can find. The macOS job also mounts the disk image
and looks for the app in it.

Two findings worth keeping, both about building rather than about CI:

- **RHEL-family systems need EPEL.** SDL fails its configure without XScrnSaver,
  and `libXScrnSaver-devel` is in EPEL and nowhere else on RHEL 10;
  `ninja-build` and `libdecor-devel` come from CRB. See `ext/README.md`.
- **MSVC's partial C23 was not a problem.** It was the largest named risk here
  and it built and tested clean first time. `windows-clang` stays as insurance
  rather than as a plan.

The sanitized preset builds and passes clean under address and UB sanitizers,
tests and renderer both. UBSan found two left-shifts of negative values — one in
`gs_fix_div`, one sampling a corner height below the datum — which are exactly
the kind of thing that is fine on one machine and not on another, in exactly the
code that must never differ between machines. Both are multiplies now, and the
golden hash did not move, which is how you know the fix changed nothing but the
standard-conformance.

---

## What does not exist

- **A race.** Gates exist and can be crossed, but nothing counts laps, times a
  run, decides a winner or shows any of it. There is no HUD.
- **Collision between cars**, and every hazard, weapon and destruction-mode rule.
- **AI.** No opponent of any kind.
- **Ghosts, the track analyser, track sharing, rollback netcode.** All of them
  are downstream of determinism, which is why determinism came first, but none
  of them are started.
- **Surface wear and persistent wreckage.**
- **Art.** No meshes, no textures, no font — the cars are coloured boxes.
  Where it will come from and how each source gets pinned is in `ASSETS.md`;
  none of it is fetched yet.
- **Sound and music.** Nothing plays.
- **A front end.** No title, no race setup, no vehicle choice, no results.
- **Shipped tracks.** The format, the editor and the route all work; nobody has
  authored a track worth shipping with them. The demo track is hard-coded in
  `main.c` and is a prototype.
- **Any release.** CI and packaging work; nothing has been tagged or published.
- **A player's guide.**

---

## The decisions, and why

Settled unless something under *Known risks* forces a change. This is the detail
`FEATURES.md` deliberately keeps out.

### The simulation links nothing

`src/core/` is a standalone C library. Every good idea in `FEATURES.md` is
downstream of it: replays, ghosts, the editor's background ghost re-racing a
track while you edit it, the headless analyser sweeping a track across gravity
values in CI, and rollback netcode. Each needs the simulation to run where there
is no window. `gearstick_cli` linking it and nothing else is the standing proof.

### The simulation is integers

Q16.16 in an `int32_t` with `int64_t` intermediates. One unit is one tile; one
tile is four metres.

Floating point is refused because determinism is the product. x87 excess
precision, FMA contraction and one libm's last bit in `sinf` each give two
machines that agree for ninety seconds and then disagree by a car length, which
breaks replays, cross-machine ghosts, content-hashed tracks and rollback
simultaneously and silently.

Trigonometry comes from a generated, committed table, because a table built at
start-up is a table a different libm can build differently.

### Both rules are checked, not remembered

`cmake/Layering.cmake` fails the configure if anything under `src/core/` includes
SDL, includes a presentation layer, or names `float` or `double` outside a
comment. Verified by deliberately breaking both.

### The world state has no pointers

A snapshot is a `memcpy`. That is what makes rollback, replay scrubbing and the
live editor ghost cheap rather than a rewrite, and it is the strongest argument
for C here: the state is plain old data, and the language's supposed weakness —
no ownership machinery — is the asset.

The track is *not* in the world. It does not change during a race, and copying
22 KB per rollback frame for something immutable would be silly.

### Cars are meshes, not sprites

The camera is a fixed orthographic isometric and stays that way — it is what
makes a two-car collision legible, which is the reason the view exists. What it
looks at is real three-dimensional geometry.

The deciding argument is a feature of this game rather than a preference.
Terrain is per-corner heights, so a car's orientation is continuous in heading,
pitch *and* roll — it leans into a banked turn and noses up a ramp. A sprite
atlas must quantise all three: six vehicles across 32 headings, five pitch
steps, five roll steps and three damage states is over fourteen thousand frames
before a wheel turns. The original never faced this because its terrain was
fixed templates with limited slopes; arbitrary player-built elevation is our own
feature breaking the sprite path.

The ground is already emitted as projected 3D geometry, so drawing cars the same
way is the same operation on a different vertex set rather than a second
renderer. It also deletes a great deal of planned work: no pre-render step, no
sprite atlas, no atlas baker, and still no image decoder. See `ASSETS.md`.

### Rendering is SDL_Renderer, not SDL_GPU

Shaded terrain geometry and textured quads is essentially the whole renderer,
and `SDL_RenderGeometry` does it at a fraction of the code of GPU pipelines and
shaders. Terrain is emitted as geometry rather than drawn from a tile atlas, so
arbitrary elevation joins stitch by construction — and per-tile gravity and,
later, surface wear become another input to the tint rather than new art.

The render layer stays thin and behind an interface, so SDL_GPU remains a later
swap if the sprite combinatorics ever justify it.

### One dependency

SDL, and nothing else. `SDL_image` was checked out for this and removed before
it was ever built, because nothing decodes an image yet — see `ext/README.md`.
Audio will be generated rather than sampled. The editor's immediate-mode UI is
the one dependency this project plans to add, at Phase 4.

### Target platforms

Linux x86_64 (RHEL and Debian families), Windows x64, macOS arm64. All 64-bit,
enforced at configure time. C23 needs GCC 14+, Clang 19+, AppleClang 16+ or
MSVC 19.39+; MSVC's C23 is partial, so `windows-clang` exists as the documented
fallback rather than as a discovery made late.

---

## Known risks

- **The feel is unproven.** The physics is correct against its own closed form,
  which is not the same as fun. That question opens the moment there is a track
  worth driving and a lap worth setting, and it is the one thing here that
  cannot be derisked by architecture.
- **The editor is roughly half the work** and is the usual cause of death for
  projects shaped like this one.
- **The editor's UI toolkit is decided and pinned, and its cost is real.**
  The currency check is done and passed: Dear Bindings v0.21 pairs with Dear
  ImGui v1.92.9b as of August 2026, and the SDL3 and SDL_Renderer3 backends are
  both supported. cimgui is alive too. What the check *also* turned up is the
  part the plan had not accounted for: Dear ImGui is C++, so a C binding is an
  API over C++ and the project would need `LANGUAGES C CXX` and the C++ standard
  library linked on all three platforms. That is a real departure from a
  deliberately C23 project and from "prefer no dependency to a small one", and
  it is weighed against the fact that the panels, palettes and dial-tweaking an
  editor needs are the thing C is worst at and the thing that kills projects
  shaped like this one.

  That cost is accepted: `ext/imgui` at `v1.92.9b` and `ext/dear_bindings` at
  `v0.21` are pinned submodules. Nothing links them yet — the first Phase 4
  items are pure C and needed regardless, so the build gains C++ when there are
  widgets to draw and not before. The simulation is untouched either way:
  `src/core/` links nothing and `gearstick_cli` links only the simulation, and
  both of those are checked rather than asserted.
- **The damage model has one shape and few numbers.** It is tested for direction
  — a downhill landing hurts less than a flat one — and not for feel.
- **`GS_MAX_CARS` is 4 and baked into the replay format.** Changing it later
  changes every recorded replay, which is why it is 4 now rather than 2.
- **`SDL_Renderer` has no depth buffer**, so once cars are meshes their
  triangles sort by painter's order alone. Boxy near-convex silhouettes should
  hold; when they do not, `SDL_GPU` is the swap the thin `src/gfx/` interface
  exists to allow.

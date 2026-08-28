# Working conventions

## The shape of the thing

Gearstick is a **construction set that happens to contain a racing game**. The
editor is not a feature of the game; the game is what the editor is for. Every
scheduling question resolves against that: the editor is roughly half the total
work, it is built alongside the racing rather than after it, and a change that
makes racing better by making authoring worse is the wrong change.

The ethic inherited from *Racing Destruction Set* (EA, 1985) is: **everything is
a dial, nothing is locked, the model is simple enough to predict and therefore
exploit, and the chaos is the reward.** A feature that adds fidelity at the cost
of the player being able to predict what the car will do is refused, however
impressive it looks. See `docs/FEATURES.md`, which keeps the rejected ideas and
their reasons so they do not get re-proposed.

**The simulation is deterministic; the presentation is not.** The world advances
in fixed 120 Hz steps inside `gs_world_step`. Frames are drawn whenever the
machine can manage and interpolate between the last two states. Do not blur this
line — physics that runs per frame is a desync with extra steps.

## Discipline

- **`src/core/` is the simulation and it links nothing.** Not SDL, not libm, not
  a renderer — not even for memory or logging. It is a standalone C library, and
  `gearstick_cli` linking it *and nothing else* is the proof. Every good idea in
  the feature list is downstream of this: replays, ghosts, the editor's
  background ghost, the headless analyser, rollback netcode. Each needs the
  simulation to run where there is no window. Checked at configure time by
  `cmake/Layering.cmake`, not remembered.
- **No floating point in `src/core/`. Ever.** Q16.16 fixed point in an `int32_t`
  with `int64_t` intermediates on every multiply. Determinism is the product
  here, not a nicety: x87 excess precision, FMA contraction and one libm's last
  bit in `sinf` each give you two machines that agree for ninety seconds and
  then disagree by a car length, which breaks replays, cross-machine ghosts,
  content-hashed tracks and rollback all at once and does it silently. Also
  checked by `cmake/Layering.cmake`.
- **The world state contains no pointers**, so a snapshot is a `memcpy`. That
  one property is what makes rollback, replay scrubbing and the live editor
  ghost cheap instead of a rewrite. A pointer added to `gs_world` costs all
  three.
- **Warnings are errors, in every build type.** `-Wconversion` and
  `-Wsign-conversion` included — in fixed-point code a silent narrowing is a
  desync rather than a crash, so they are the two that matter most here. First
  party targets only; vendored code in `ext/` is untouched.
- **Verify on the real output.** A frame written by `--shot`, a `ctest` run, a
  state hash from `gearstick_cli` — not a proxy, and not "it should work now".
- **The golden replay is the tripwire.** `gearstick_cli selftest --verify`
  re-races a fixed input log and compares one hash. If a change to the physics
  moves it, that is not a test being annoying: every ghost time and every shared
  replay in existence just became wrong. Changing the hash is a deliberate act
  with a note in `PROJECT_STATUS.md` saying why.
- **Content is data, not C.** Tracks, vehicles and the dials belong in files the
  editor writes and the game reads. A hard-coded track is a prototype and must
  be replaced before anything is built on it.
- **One item at a time, landing with its test.** Keep `ctest` green; a red tree
  is the stop-everything condition. Name tests as sentences stating the fact
  they pin: `a_car_left_on_a_slope_rolls_downhill`.
- **A test that only sometimes tests its rule is worse than no test**, because
  the green tick is not evidence. Build the situation explicitly rather than
  depending on what a generator happened to produce.
- **Cover every scenario, not a representative sample of them.** The standard is
  100%: every control, every dial value, every combination, every state a screen
  can be in. Sampling is what has been done up to now and it is why faults keep
  reaching a person clicking around instead of a red tree. **Do not size the
  claim to fit a slow test — fix the test.** The bound that makes exhaustive look
  unaffordable is nearly always a step doing work it does not need: rasterising
  a frame to read back a menu, or replaying a path it could have snapshotted.
  Make the step cheap, then walk all of it.
- **Coverage is asserted, not believed.** A test that walks a space states how
  big the space is and how much of it it covered, and fails when those differ —
  so a control added next month and never walked turns the tree red by itself.
  Anything genuinely left out is named in the test, with its reason, where the
  next person will read it.
- **Commit each finished item and push.**
- **Every commit that lands an item updates both living docs in that same
  commit**: `docs/PROJECT_STATUS.md` (what now works, with its verification) and
  `docs/COMPLETION_PLAN.md` (tick the item, add any tails found on the way).
  Re-read both in full at every phase boundary — status docs rot fast.
- **`COMPLETION_PLAN.md` is a user document. Keep it a summary.** One or two
  sentences per item and a verification in plain English — "halving gravity
  doubles the jump distance", not a list of function names. Implementation
  description, test-name inventories and design rationale all go in
  `PROJECT_STATUS.md`, which is where anyone wanting detail is sent.
- **`FEATURES.md` has no implementation in it.** It is the high-level menu and
  stays that way; the moment a data structure appears in it, it has stopped
  doing its job.
- **Never describe a partial module as working.** "Working" means 100% of what
  it claims. Anything less is reported with the missing part named *first* —
  "the terrain samples continuously; nothing draws it yet", never "the terrain
  works". If the answer to "is this done?" needs a qualifier, the qualifier is
  the honest report.

## This game's specifics

- **One world unit is one tile.** Positions, heights and radii are all in tiles,
  so a slope is a ratio and never a pixel measurement. Pixels exist in `src/gfx/`
  and nowhere else.
- **Angles are a full turn in 65536 steps in a `uint16_t`.** Wrapping is the
  type's job. There is no modulus in the physics and there should never be one —
  a `while (a > TAU)` is a compiler reassociation away from being wrong.
- **Trigonometry comes from a generated, committed table.** A table built at
  start-up is a table a different libm can build differently. Re-generate with
  `tools/make_tables.py` and commit the result.
- **A default track is long.** Every track that ships must be at least ten to
  twenty times the route length of the August 2026 set, which measured 28 to 173
  tiles and averaged 63 — so **630 to 1260 tiles of route**, not a start, a jump
  and a finish. This has been asked for repeatedly and lost repeatedly; it is a
  hard acceptance criterion for anything touching the generator or the shipped
  set, and `tools/make_tracks.c` refuses a track shorter than
  `GS_STOCK_MIN_ROUTE`. Note that `GS_TRACK_MAX` bounds the world at 64x64
  tiles, which cannot hold such a route: the cap is part of the work.
- **Gravity is sampled, not read.** It is a per-tile field with a global scale
  over it — the gravity brush — so the physics asks the track what gravity is
  *here* rather than consulting a race setting. Anything that caches gravity for
  a whole race breaks the feature.
- **Terrain is per-corner heights, sampled bilinearly.** Ramps stitch
  continuously and a surface normal falls out for free. Nothing anywhere may
  assume a tile is flat.
- **Terrain is drawn as shaded geometry, never from a tile atlas.** Arbitrary
  elevation joins are then correct by construction, and per-tile gravity and
  surface wear become another input to the tint rather than new art.
- **The shadow is what sells the jump.** An airborne car is only readable
  because of the gap between it and its shadow on the ground. This was true on a
  C64 and it is still the single most important pixel in the frame.
- **Prefer no dependency to a small one.** SDL_image decodes PNG through stb
  rather than libpng, because libpng drags in zlib and a system zlib advertising
  a static library it does not ship fails a configure with an error naming
  neither.

## Layout

```
src/core/       the simulation - links nothing, integers only, no pointers in state
src/gfx/        isometric projection, terrain geometry, sprites
src/ui/         menus, HUD, race setup
src/platform/   asset and save paths, keyboard and gamepad
src/frontend/   one main.c per executable: game/ and cli/
cmake/          platform gate, warning set, layer and float checks
tools/          table baker, art pipeline
ext/            pinned submodules - see ext/README.md
docs/           FEATURES.md, COMPLETION_PLAN.md, PROJECT_STATUS.md
```

# Completion plan

The road to done, in phases. **This is the map, not the territory** — one line
per item, saying what it is and how you would know it works. What any of it is
actually made of lives in `PROJECT_STATUS.md`, which is the file to read when
you want detail. Why a feature exists at all lives in `FEATURES.md`.

**`[x]` means 100% of the item, and nothing less.** Not "the parts the demo
exercises", not "enough to move on". An item with any claimed behaviour
unimplemented stays `[ ]`, with the missing part named in its text. Splitting an
item to tick the easy half is the same violation wearing a different shape.

Every item names its verification. An item without one cannot be ticked. Tails
found while implementing something go in at the bottom the moment they are
found, not when someone remembers.

`[x]` done · `[ ]` not started, or **In progress** where the text says so

**Done means every phase item is ticked.** Not "the current one is finished", not
"progress is orderly" — those are how the work is done, not whether it is. The
plan is finished when this returns nothing:

```sh
sed -n '/^## Phase /,/^## Tails/p' docs/COMPLETION_PLAN.md | grep '^- \[ \]'
```

The tails below the phases are found-work rather than planned work, and are
counted separately.

**Phases 0 to 4 are complete — 46 of 61 items, every one of them with its
verification actually run.** The editor was about half the remaining work and
it is done: a track can be built, painted, given a route, validated, driven and
undone, from a mouse or from a pad. Phase 5 is next.

Where a verification says a deliberate bug "turns it red", that is not a figure
of speech: the bug was introduced, the test was watched to fail, and the bug was
reverted. A test nobody has seen fail is a test nobody should trust.

The phase order is not arbitrary. Determinism comes before everything because
ghosts, replays, the editor's live ghost, the track analyser and rollback are
all downstream of it. The feel of a car in the air comes before the renderer
because it is the only part of this project that might turn out not to be fun,
and finding that out cheaply is worth more than a pretty prototype.

---

## Phase 0 — Foundations

- [x] **C23 + CMake/Ninja build**, presets per platform with matching test
      presets. *Verification: every preset on the host configures, builds and
      tests.*
- [x] **64-bit-only and C23 gates**, with `nullptr` probed rather than inferred
      from a version number. *Verification: the configure line names the
      resolved platform, compiler and standard.*
- [x] **Warnings as errors in every build type**, first-party targets only.
      *Verification: both presets build clean from a wiped tree.*
- [x] **The simulation links nothing** — no SDL, no libm — and no floating point
      appears in `src/core/`, both checked at configure time rather than by
      review. *Verification: a deliberate `#include <SDL3/SDL.h>` and a
      deliberate `double` each fail the configure with a message naming the file
      and the line; `ldd gearstick_cli` lists libc and nothing else.*
- [x] **CI on all three platforms** — Ubuntu, Rocky, Windows and macOS arm64 —
      building, testing and running the headless driver. *Verification: all four
      jobs green on `main`, and with them the job that depends on all four —
      four platforms, three compilers, two architectures, one state hash.*
- [x] **Packaging** — a tarball for Linux, a zip for Windows, a disk image for
      macOS, each unpacking to something that runs in place.
      *Verification: the `package` workflow unpacks each artifact into a
      different directory on its own platform, re-races the golden replay out of
      the unpacked copy, and draws a frame from it — which is the step that
      proves the packaged layout is one the asset probe can find. The macOS job
      also mounts the disk image and finds the app inside it.*
- [x] **The three living documents exist and are honest** — this plan,
      `PROJECT_STATUS.md` and `FEATURES.md`. *Verification: someone who has not
      seen the code can say what works from `PROJECT_STATUS.md` alone.*

## Phase 1 — The feel

The prototype that answers whether this is a game. Flat ground, one surface, one
car, no art worth the name.

- [x] **Fixed-point arithmetic and trigonometry**, with the tables generated and
      committed rather than computed at start-up. *Verification: the trig agrees
      with double precision to within 1e-4 across a full turn and `atan2`
      inverts it to a fifth of a degree; re-running the baker reproduces the
      committed table byte for byte.*
- [x] **A car that drives on flat ground** — throttle, brake, steering that
      depends on speed, and a grip limit it can be driven past.
      *Verification: throttle reaches a cruise and the brake stops it and then
      reverses it; a car turns more sharply at a crawl than at speed; ice lets
      go of a slide long after pavement has caught it.*
- [x] **Fixed 120 Hz simulation step with an accumulator**, decoupled from the
      frame rate. *Verification: one second of wall clock chopped into 30, 60,
      144 and 240 frames each deliver exactly 120 ticks, and a race paced
      through the clock hashes equal to the same race stepped directly. A
      ten-second stall delivers a quarter second of catching up rather than 1200
      ticks.*
- [x] **The simulation runs headless**, driven by the CLI with no window and no
      audio device. *Verification: `gearstick_cli` links libc and nothing else.*
- [x] **Input-log replays**, recorded and replayed to an identical end state.
      *Verification: a golden replay — inputs in, one state hash out. A 1%
      change to pavement grip turns it red with the hash it wanted and the hash
      it got.*
- [x] **A world snapshot is a memory copy.** *Verification: save, run three
      hundred ticks, restore, re-run the same inputs, and the two end states
      hash equal.*

## Phase 2 — Elevation and air

- [x] **Terrain with per-corner heights**, sampled continuously so ramps stitch
      without seams. *Verification: ground height a thousandth of a tile either
      side of a tile join differs by less than the sampling error — there is no
      step.*
- [x] **Airborne flight and landing.** Leaving the ground, flying ballistically,
      and touching down. *Verification: the range of a jump is within 5% of the
      closed-form prediction from its take-off velocity and the gravity it flew
      under.*
- [x] **Gravity as a race parameter, continuous, with the planet presets named.**
      *Verification: halving gravity doubles the jump distance from the same
      take-off, to within 5%.*
- [x] **Slopes affect the car on the ground** — climbing, accelerating downhill,
      and sliding on a steep enough face. *Verification: a car left at rest on a
      slope accelerates downhill and one on the flat does not move at all.*
- [x] **Landing quality matters.** What costs damage is the mismatch between how
      fast the car is coming down and how fast the ground is falling away
      beneath it — not raw falling speed. That is the whole reason to shape a
      landing. *Verification: the same jump landed on the flat and landed on a
      downslope produce different damage.*
- [x] **Gravity per tile — the brush.** *Verification: a car crossing a painted
      one-fifth-gravity stripe mid-jump lands more than 40% further than the
      same jump without it.*

## Phase 3 — The isometric renderer

- [x] **Terrain drawn as shaded geometry**, lit by slope, tinted by surface, so
      arbitrary elevation joins are correct by construction.
      *Verification: frames captured headless show a ramp, a bowl and three
      surfaces with no seam and no hole.*
- [x] **Cars drawn with correct depth order** against the terrain and each other.
      *Verification: a car behind a five-tile rise contributes no pixels at all,
      where the same car on open ground contributes hundreds. Drawing cars after
      the terrain instead of among it turns that red.*
- [x] **A shadow projected down to the ground under an airborne car.**
      *Verification: captured frames through a jump show the gap between car and
      shadow opening and closing with height.*
- [x] **Sub-pixel camera motion.** *Verification: a car driven across a tile
      boundary in equal steps produces frame-to-frame differences with no single
      step more than three times the mean. Quantising the camera to whole tiles
      — the bug that makes the world lurch 32 pixels at every boundary — turns
      that red.*
- [x] **Render interpolation between simulation ticks.** *Verification: drawn at
      a third of the way between two ticks, the car's centroid is a third of the
      way between the two positions, within a pixel of rasterisation.*
- [x] **A frame can be captured from the command line**, so a change to the
      renderer can be looked at rather than described. *Verification: `--shot`
      writes a frame under the dummy video driver and the software renderer.*

## Phase 4 — The editor

Half the project. Not deferred, not bolted on.

**Reordered from the original list**, because it had a dependency inversion: the
first item's verification was "a track built in the editor saves, reloads and
races", which needs the file format that was item seven. The order below builds
the data model first — all of it pure C in `src/core/`, and all of it needed
whatever the UI turns out to be — then the interface on top.

- [x] **A track file format, and identity by content.** Save, load, and a
      version stamp. *Verification: a track round-trips through a real file on
      disk and hashes equal; two tracks built independently to the same design
      produce the same identifier; a one-tile edit produces a different one; a
      short read, a truncated payload, a bad magic and a version from the future
      are each refused with the caller's track left untouched.*
- [x] **An edit model with undo and redo**, unlimited within a session.
      *Verification: five hundred mixed edits undone completely return the track
      to its starting hash and redone return it to the edited one; a forty-tile
      brush stroke undoes as one action; a new edit drops the redo tail; edits
      that change nothing leave no step in the history; and a full log refuses
      the edit rather than applying one it could not take back.*
- [x] **A grid cursor and a piece palette.** *Verification: a track built with
      the brushes — a ramp raised a strip at a time, a field of ice, a painted
      low-gravity pocket — saves, reloads to an identical hash, and is then
      driven over: the car climbs what was built and leaves the ground at the
      top of it. The cursor is pinned separately by a round trip: project a
      known point, pick the pixel it landed on, and land back within a hundredth
      of a tile.*
- [x] **Raise and lower elevation as a brush.** *Verification: one application
      moves the ground by exactly the step in the panel, up or down, and below
      the datum as readily as above it. A ramp drawn at a quarter tile per tile
      measures as a quarter tile per tile, throws a car off its crest at that
      exact gradient, and the jump lands where that launch predicts to within
      six percent.*
- [x] **Paint surface, and paint gravity.** *Verification: two identical cars
      driven identically over two identical tracks differing only by the paint.
      Pavement catches a four-tile-per-second slide inside two seconds; a
      painted ice field does not. A painted one-third-gravity pocket over a
      landing lengthens the jump by more than half, without the race gravity
      changing at all.*
- [x] **A route: a start line and ordered gates.** A track is terrain — heights,
      surface, gravity — and terrain alone does not say which way round it goes
      or where a lap begins. *Verification: gates are placed with the editor's
      gate brush where the pointer is, in the order clicked, removable with the
      order behind them intact, and saved, reloaded and hashed with the track —
      the same ground with a different route is a different track. A car driven
      through a gate is seen to cross it; one driven past the end of it, or
      backwards through it, or stopping short of it, is not.*

      **Added after the fact.** Validation was the next item on the original
      list, and writing it made plain that there was nothing to validate: "is
      the loop closed" presumes a loop, and no part of a track said which way
      round it went. Gates rather than road tiles, because the terrain here is
      free-form — a bowl, a plateau, a jump to nowhere are all buildable — and a
      ribbon of road tiles would insist the drivable part is a ribbon. Authored
      order also beats inferred order on the one axis this game cares about
      most: a player can predict it.

- [x] **Track validation** — is there a start, are the gates in a closed order,
      is every one of them reachable ground. *Verification: `gearstick_cli
      validate` builds a sound route and each way of breaking one, and exits
      non-zero if a sound route is refused or a broken one accepted. Five
      problems, each named and each pointing at the gate at fault: no start
      line, a start with nowhere to go, a gate hanging off the edge, a gate too
      narrow to drive through, and two gates in one place. The editor shows the
      same verdict continuously rather than on demand.*

      *Completability — "can a car actually get round it" — is not here. It
      needs something that drives, so it belongs with the analyser in Phase 9,
      and claiming it as validation would be claiming a check nothing performs.*
- [x] **Instant test-drive from the cursor, and snap back.** *Verification: Tab
      drops a car where the pointer was, facing the way the start line does, on
      the very track object being edited — nothing is written, read or copied.
      Coming back leaves the track hash and the undo history exactly as they
      were, and returns the camera to the part of the track being built rather
      than to wherever the car stopped.*
- [x] **The live ghost** — a car continuously re-racing the design as it is
      edited. *Verification: a ghost races a flat track; a ramp is then drawn in
      its path with the brush and **nothing is told anything** — it notices by
      the track's own hash and is somewhere else entirely the same number of
      ticks into its next run. Undo the ramp and it goes back. Driven the way
      the frontend drives it, two ticks a frame, because a ghost that restarts
      once per call looks perfect under one big call and never moves at all
      under the real thing.*
- [x] **The editor UI is usable with a pad as well as a mouse.**
      *Verification: a track built from a pad with no pointer involved — the
      stick moves the cursor, the south button paints a stroke that undoes as
      one action, the shoulders undo and redo on the press rather than the hold,
      the west button cycles every brush including the gate one, and start asks
      for a test drive. The panel itself walks under ImGui's gamepad
      navigation, and the pad is ignored while that is using it.*

## Phase 5 — Surfaces, vehicles and dials

- [x] **Three surfaces with distinct grip**, on the ground and in the corners.
      *Verification: the same corner, same speed, same steering. On pavement the
      car goes where it points — under two degrees of slip — and gets round.
      On ice it points into the corner and carries straight on, thirty degrees
      of slip and less than a third of the turn. Dirt sits between them on both
      counts.*
- [x] **A vehicle roster with real trade-offs** — engine, tyres, mass.
      *Verification: `gearstick_cli roster` races all six over ten conditions
      chosen to be decided by different things, and fails if any vehicle is best
      at nothing. Every one of them wins something: the sprint car where there
      is grip to spare, the motorcycle where there is not, the rover where
      gravity has taken nearly all of it, the stock car off a shelf, the buggy
      on rough ground that needs turning, and the baja bug where the ground is
      breaking everything else.*

      *The bar is "every vehicle wins something", not the plan's original "no
      vehicle wins everything" — two winners and four also-rans passes the
      second and is still a roster of two. A machine that is best at nothing is
      a choice nobody would make.*
- [x] **The dials: gravity, air drag, friction scale, damage multiplier**, all
      continuous, all changeable before a race. *Verification: each is swept
      across its range against the same input log — two points would pass a dial
      with three settings pretending to be a slider. More drag is strictly less
      speed at every step; more gravity is strictly less jump; more damage is
      never less damage; more friction turns the car further until grip stops
      being the limit and the steering starts, which is pinned as a plateau
      rather than asserted past. They live in the editor beside the brushes, and
      the ghost re-races the moment one moves.*
- [x] **Surface wear over a race.** *Verification: the same corner driven five
      times on dirt comes round measurably less than the first time — the line
      has churned into ruts. Pavement is unmoved by any number of laps, to
      within a thousandth of a degree. Ice polishes into something both faster
      and looser. A sliding tyre marks the ground harder than a rolling one,
      which is why the racing line goes off before the rest of the track. And
      wear belongs to the race rather than the track: it is hashed as world
      state and travels in a snapshot, but a track's identity does not drift
      because somebody drove on it.*

## Phase 6 — Two players, then four

- [x] **Split-screen for two**, on one machine. *Verification: pad N drives car
      N, a pad nobody plugged in drives nothing, and the keyboard is added to a
      pad rather than substituted for it — so one keyboard still drives two cars
      when there are no pads at all. Two cars given opposite inputs turn
      opposite ways and end a long way apart, and each half of the screen
      contains its own car and not the other one.*
- [x] **Four-player split-screen.** *Verification: one, two, three and four
      views tile the window without overlapping or leaving it, and three uses the
      same grid as four so a player joining does not rearrange everybody else's
      screen. Each view contains its own car and none of the others. And four
      views cost less than three times one full-window view — measured, not
      assumed, which is how the missing culling was found.*
- [x] **The merging camera** — views combine into one when the cars are close
      and separate when they are not, without a visible seam at the transition.
      *Verification: close cars get one view, far cars get one each, and a pair
      jiggling across the threshold for six hundred frames never changes the
      count once — two thresholds, not one. The seam is measured rather than
      judged: what is checked is not how far the camera moves in a frame, since
      during a transition it moves quickly on purpose, but how much that changes
      between frames. Under a tenth of a tile throughout. An instant switch, an
      unsmoothed blend, and a single threshold each turn it red.*
- [x] **Every control remappable, and the game fully playable from a pad
      alone.** *Verification: every player has a complete pad layout out of the
      box — the fourth of them should not have to configure one before playing —
      and pressing all five of a player's pad buttons produces all five actions.
      Any control moves to any key or button; binding one that another action on
      the same player already uses takes it away from that one, because two
      actions on one button is a scheme nobody meant to make and would be found
      mid-corner. A control can be cleared outright, not merely moved. Keyboard
      and pad both count at once, so there is no mode to switch. Changed controls
      survive a write and read, and a corrupt file leaves the player driving with
      what they had. The rebinding panel itself is walkable with a pad, as is the
      whole editor.*

## Phase 7 — Destruction

- [x] **Car-to-car collision that launches rather than punishes.**
      *Verification: a head-on at five tiles a second sends both cars back the
      way they came with more speed than they arrived with — the bounce is over
      one on purpose, because a hit that costs two seconds and teaches nothing
      is the punishment this design refuses. A harder one puts them in the air.
      Their closest approach is watched rather than where they finish, since
      cars that pass through each other and are shoved apart afterwards look
      identical at the end. Cars already overlapping and not closing are pushed
      apart, which no impulse can do. A car flying overhead does not touch the
      one below. Three runs of the same collision hash identical.*
- [x] **Damage and wrecking**, from collisions and from bad landings.
      *Verification: a motorcycle driven off a cliff on an empty track destroys
      itself; two cars driven repeatedly into each other on flat ground destroy
      the fragile one, where the ground can take no credit. A gentle shunt keeps
      both on the ground and still costs something, which is how the two sources
      are known to be separate. A wreck is scenery: it does not move, and the
      living bounce off it.*
- [x] **Droppable hazards.** *Verification: somebody else's oil halves the
      corner a car can take; your own does nothing to you at all, because
      driving into what you dropped would make the weapon a way of hurting
      yourself and nobody would use it. Grip comes back the moment you are off
      it — a slick is driven through, not served like a penalty. A mine goes off
      once, launches and hurts whoever found it, and is gone for the next car. A
      held button leaves about five hazards in five seconds rather than six
      hundred.*
- [ ] **Destruction mode.** *Verification: a race ends correctly when one car is
      left driving.*
- [ ] **Wreckage that persists as track geometry.** *Verification: a wreck
      changes the racing line for the remaining laps.*

## Phase 8 — Opponents

- [ ] **AI that completes any valid track.** *Verification: the analyser drives
      every shipped track to completion at default settings.*
- [ ] **AI that re-plans for the current gravity and vehicle** rather than
      following a baked speed profile. *Verification: the same track at 0.4g and
      1.8g produces different braking points.*
- [ ] **AI that is beatable and worth racing.** *Verification: stated lap times
      against a human baseline on the shipped tracks.*

## Phase 9 — Sharing, ghosts and netcode

- [ ] **The track analyser sweep**, reported as an envelope and drawn as a
      heatmap over the editor. *Verification: it correctly calls an impossible
      jump impossible.*
- [ ] **Ghosts** — race against a recorded run, yours or someone else's.
      *Verification: a ghost replays identically on another machine.*
- [ ] **Track sharing as a code or URL.** *Verification: a track round-trips
      through the code and hashes equal.*
- [ ] **Rollback netcode for two players online.** *Verification: a race under
      simulated latency and packet loss ends in the same state on both
      machines.*

## Phase 10 — Presentation and release

- [ ] **The art pipeline** — vehicles and surfaces generated rather than
      hand-drawn, with attribution generated in the same run.
      *Verification: re-running the pipeline reproduces the committed output.*
- [ ] **Sound.** Engine note that tracks the drivetrain, surface-dependent tyre
      noise, impacts, and the silence of being airborne.
      *Verification: listened to, on all three platforms.*
- [ ] **Music.** *Verification: as above.*
- [ ] **Front end** — title, race setup, vehicle choice, results.
      *Verification: a full session start to finish without touching a command
      line.*
- [ ] **A player's guide**, covering the first ten minutes, every control, the
      editor and how to report a bug as a file somebody else can run.
      *Verification: someone else follows it and races.*
- [ ] **Signed or clearly-documented-unsigned releases** for all three
      platforms. *Verification: downloaded and run on a machine that has never
      had a toolchain on it.*

---

## Tails

Found while implementing something else. Added when found, not when remembered.

- [ ] **What surrounds a track is undecided.** There is no wall at the edge — a
      car that drives off continues onto a plain that carries on at the height
      and surface of the nearest edge tile, so leaving the track currently costs
      nothing at all. It should probably cost something.
- [ ] **A hard-coded demo track is in the game frontend.** It is a prototype and
      it goes the moment the editor can write a real one.
- [ ] **Nothing in the game frontend is a race.** No start, no laps, no
      checkpoints, no finish, no HUD — two cars drive around on a demo track.
      Lap order is a Phase 4 item because it is derived from the track, but the
      frontend needs its own item to actually run a race.
- [ ] **`assets/` is empty and exists only so the install rules have something
      to copy.** Fine now; a smell if it is still true at Phase 10.
- [ ] **Placing a gate is not in the undo history.** The undo model records a
      cell changing from one value to another, and a gate is a list entry rather
      than a cell. Removing a misplaced gate is easy, so this is an
      inconsistency rather than a hole — but it is one, and undo that covers
      most of what you did is a promise with a footnote.
- [ ] **Import a corpus of real tracks to test against.** Our own tracks will
      all have been built by whoever wrote the editor, which is the worst
      possible sample. The *Stunts* corpus is the target — documented format,
      grid-based with elevation and three surfaces, hundreds of community
      tracks — and it belongs with the analyser in Phase 9, where "is this
      completable" is the question being asked. Its repository's licence is
      unclear and must be checked first. Reference only; nothing imported ships.
- [ ] **Read the original's manual for its designers' notes on the 50 stock
      tracks.** They say what each track was *for* — one built to aim both
      drivers at each other on pavement, another the shortest buildable track so
      you do not have to go far to find someone to hit. That is design rationale
      from the authors, it needs no reverse engineering, and it should inform
      our own stock tracks before they are built.

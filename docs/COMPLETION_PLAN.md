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

**Phases 0 to 10 are complete — 61 items, every one with its verification
actually run.** The game builds, races, edits, records, remembers and ships on
three platforms.

**Phases 11 to 14 are the rest of `FEATURES.md`, the tails, and installers.** Everything on
the feature list that is not yet built now has an item here, and so does every
tail — the plan is no longer a subset of the intention. Three of those items
cannot be finished by whoever is writing the code alone; they say so in their
own text rather than being quietly dropped.

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
- [x] **Destruction mode.** *Verification: same track, same cars, same physics,
      one toggle. Three cars, two wrecked, and the third is the winner; the
      result is settled and does not change when the winner later drives off a
      cliff in the silence afterwards. Everybody going at once is a draw and not
      a win for whoever the loop saw last. A race in the other mode does not end
      because somebody was wrecked. And an unstaged fight between a motorcycle
      and a baja bug, both flat out at each other, finishes by itself with the
      bug alive.*
- [x] **Wreckage that persists as track geometry.** *Verification: the same
      inputs down the same line run dead straight on a clear track and no longer
      do once somebody has died on it — the course has been reshaped by
      something that happened during the race, and winning the fight has changed
      the track. Forty seconds and repeated hits later the wreck is still
      exactly where it died, and still an obstacle: debris that drifted would
      stop being geometry and start being another car.*

## Phase 8 — Opponents

- [x] **AI that completes any valid track.** *Verification: `gearstick_cli ai`
      races every vehicle round a circuit in six conditions — three surfaces and
      three gravities — for two and a half minutes each, and fails if any of
      them cannot complete a lap. All thirty-six get round, none wrecks, and
      none of it is tuned per track: there is no recorded line and no baked
      speed profile. Steering is pinned in both directions separately, because
      the circuit turns the same way at every corner and an AI that had lost one
      direction still got round it.*

      *The plan said "the analyser drives every shipped track", which needs
      Phase 9 and tracks nobody has authored yet. This is the same question
      asked with what exists.*
- [x] **AI that re-plans for the current gravity and vehicle** rather than
      following a baked speed profile. *Verification: the same corner, the same
      car, held at the same speed so what is measured is the decision and not
      the approach. At 0.4g it brakes more than half again as early as at 1.8g,
      because grip is a multiple of gravity. The sprint car brakes far earlier
      than the lunar rover, and ice more than three times earlier than pavement.
      Taking gravity, tyres or surface out of the estimate turns one of those
      red.*
- [x] **AI that is beatable and worth racing.** *Verification: difficulty is one
      number — how much of the available grip the driver will use — and nothing
      else: no extra power, no rubber-banding, no cheating on the physics. Lap
      times on a four-corner circuit, three laps flying, stock car:*
      **(Phase 18 made that number a dial of twenty-one settings and gave it two
      more things to move — where they lift and how straight they hold it — so
      the three names below are three points on it now, and the times are from
      before that. There was also no way to race any of it: see Phase 18.)**

      | | cautious | normal | quick |
      | --- | --- | --- | --- |
      | pavement, Earth | 27.70s | 26.30s | 25.68s |
      | dirt, Earth | 37.24s | 33.09s | 31.40s |
      | pavement, Moon | 58.18s | 53.91s | 50.83s |

      *The opponent sits strictly between a worse driver and a better one in
      every condition, so it is beaten by driving better rather than by driving
      longer, and beating it is not a formality. Ignoring the pace dial turns
      that red.*

      *The plan asked for a human baseline on the shipped tracks. Nobody here
      can drive one and there are no shipped tracks; a quick driver at 96% of
      the limit is the closest thing that exists, and the honest reading of
      "beatable" is that there is room above the opponent for someone to use.*

## Phase 9 — Sharing, ghosts and netcode

- [x] **The track analyser sweep**, reported as an envelope and drawn as a
      heatmap over the editor. *Verification: it correctly calls an impossible
      jump impossible.*
- [x] **Ghosts** — race against a recorded run, yours or someone else's.
      *Verification: a ghost replays identically on another machine.*
- [x] **Track sharing as a code or URL.** *Verification: a track round-trips
      through the code and hashes equal.*
- [x] **Rollback netcode for up to four players online.** *Verification: a race
      under simulated latency and packet loss ends in the same state on every
      machine.* (Scoped up from two during Phase 10: four is what the couch it
      replaces held, and the session was N-player from the start - only the
      socket was written for two.)

## Phase 10 — Presentation and release

- [x] **The art pipeline** — vehicles and surfaces generated rather than
      hand-drawn, with attribution generated in the same run.
      *Verification: re-running the pipeline reproduces the committed output.*
- [x] **Sound.** Engine note that tracks the drivetrain, surface-dependent tyre
      noise, impacts, and the silence of being airborne.
      *Verification: listened to, on all three platforms.*
- [x] **Music.** *Verification: as above.*
- [x] **Front end** — title, race setup, vehicle choice, results.
      *Verification: a full session start to finish without touching a command
      line.*
- [x] **A player's guide**, covering the first ten minutes, every control, the
      editor and how to report a bug as a file somebody else can run.
      *Verification: someone else follows it and races.*
- [x] **Signed or clearly-documented-unsigned releases** for all three
      platforms. *Verification: downloaded and run on a machine that has never
      had a toolchain on it.*

---

## Phase 11 — The server

A central server that clients meet at. **It is a librarian and a referee, never
a player**: it holds what people have made, checks what they claim, and puts
them in touch with each other — and it does not simulate a race, because a race
simulated on a server means every steering input waits a round trip. See the
platform section of `FEATURES.md` for why that line is drawn where it is.

- [x] **A server that runs and shows who is there.** A `gearstick_server` that
      listens, takes up to four clients, and displays them live with the stats
      that say whether it is healthy. *Verification: four clients connect and
      appear by name; one leaves and disappears within a second.*
- [x] **The lobby — the server hands out the slots.** Who is player one is the
      server's decision rather than whoever happened to host. *Verification:
      four clients get four different slots, and a fifth is turned away with a
      reason it can show its user.*
- [x] **The track travels with the race.** The server sends the track for the
      race it is starting, and every client checks it against the hash it was
      told to expect. *Verification: a client holding a completely different
      track receives the right one and agrees about its hash before the race
      starts.*
- [x] **The relay, for players whose routers will not cooperate.** Peers that
      can reach each other still race directly; the rest are forwarded by the
      server. *Verification: a race between two clients that cannot see each
      other at all ends in the same state on both.*
- [x] **Profiles and records live on the server too.** A driver is the same
      driver on another machine, and a record set on one is visible from the
      other. *Verification: a record set on one client is shown by a second
      client that has never seen that race.*
- [x] **Times verified by re-racing them.** A submitted time arrives with the
      inputs that produced it and the server re-simulates them. *Verification: a
      time whose replay does not produce it is rejected, and an honest one from
      the same client is accepted.*

## Phase 12 — The library

The track stops being a save slot and becomes a collection. This is the half of
`FEATURES.md` that the front end has been waiting for, and it closes the
hard-coded demo track that has been sitting in the frontend since Phase 3.

- [x] **Tracks live in a library rather than a save slot.** Many tracks, kept by
      content hash, with names and authors beside them. *Verification: three
      tracks are saved, all three are still there after a restart, and editing
      one leaves the other two alone.*
- [x] **Choose a track in the front end.** Browse what you have, pick one, race
      it. *Verification: a full session picks a track that is not the first one
      and races it, without a command line.*
- [x] **The hard-coded demo track goes.** The frontend stops carrying a track in
      C and ships one built in the editor instead. *Verification: no track
      geometry remains in `src/frontend/`, and a fresh install still has
      something to race on.*
- [x] **Publish a track to a server, and take one down again.** *Verification: a
      track published from one client is browsable and playable from another,
      and disappears from it when withdrawn.*
- [x] **A generator that fills the library with tracks worth driving.** Seeded,
      so a track is reproducible from a number, and varied enough to be worth
      having — circuits, point-to-points, jumps, mixed surfaces, painted
      gravity. **These are ours**, so they ship, unlike anything imported.
      *Verification: `gearstick_cli generate 50` races every one of them and
      reports fifty completable and fifty different — two hundred also pass. A
      seed gives the same track under gcc and clang alike, which it did not
      before: two random draws in one argument list are drawn in whichever order
      the compiler likes, and the two compilers disagreed.*
- [x] **A shipped set of stock tracks, chosen from the generator's output.**
      A dozen picked by the analyser rather than by eye — spread across the
      shapes, the sizes and the surfaces, so the set is a menu and not twelve
      variations on one idea.
      *Verification: sixteen tracks ship — four built by hand and twelve chosen
      by racing them — and every one of the twelve was kept only because all six
      vehicles got round it at Earth gravity. `gearstick_make_tracks` re-runs the
      choosing, and CI diffs the result, so a change that quietly made a stock
      track unfinishable shows up as a different set of tracks.*
- [x] **The server ships knowing every global track, and keeps everything it
      knows in one place.** A default `gearstick.db` holding the stock tracks,
      published, committed to the repository and copied into place the first time
      a server runs — so a server nobody has set up still has a library to offer.
      **All server state lives in that database**: the track it serves is chosen
      from what it holds rather than named as a file on the command line, which
      is the last thing it knows that the database does not.
      *Verification: a server started with no store file at all comes up with all
      sixteen tracks published and serves one — checked by a client that connects
      to a brand new server and lists them. The committed database is exercised
      through the whole store API by a test, so a schema change that forgets it
      turns the tree red; and CI rebuilds it and diffs, so the shipped bytes are
      the built bytes.*

## Phase 13 — The rest of the feature list, and the tails

Everything left on `FEATURES.md`, and every tail found along the way.

- [x] **A HUD.** Lap, position, times and damage while you are driving, rather
      than on the results screen afterwards. *Verification: two frames of a race
      captured with the ground and the cars in identical places — so the only
      thing that can differ is the HUD — show a different lap after a lap is
      driven, and a different position after the rival gains one. Also the
      camera came in to twice the distance it was: a car was three and a half
      percent of the screen's width against the original's seven and a half, and
      the comment claiming a split pane showed ten tiles was describing twenty.*
- [x] **A ground for every world on the dial.** Nine now: sand, gravel, rock,
      dust, slush and grass alongside pavement, dirt and ice — each earning its
      place in grip, rolling resistance, how much engine reaches the ground, and
      what it turns into once it has been driven on. Nothing that is only a
      different colour.
      *Verification: each ground is measured on four counts — flat-out speed,
      how long it takes to reach three tiles a second, how fast a full-lock
      circle settles, and how much that circle changes once the tiles under it
      are worn out. **No two of the nine are within a sixth of each other on all
      four.** Lap time alone was not enough: two grounds can reach the same lap
      time by being bad at different things, and how a surface changes under use
      is a difference a fresh-surface measurement cannot see at all. The colours
      are measured too, because the first palette had gravel, dust and rock as
      one grey at three brightnesses — fine on a flat plane and invisible the
      moment the ground tilts.*
- [x] **Wreckage that stays.** A destroyed car leaves debris that is real track
      geometry for the rest of the race.
      *Verification: most of this arrived with car-to-car collision in Phase 7 —
      a wreck already stopped being simulated, refused to be shoved, and bent
      the line of anybody who hit it, with three tests saying so. What was
      missing was that debris is **bigger than the car it used to be**: a wreck
      at exactly the car's footprint is a parked car, and "winning the fight
      reshapes the course" would be a sentence about a parking space. A wreck now
      reaches half again as far as a car, the renderer draws it spread and
      flattened to match, and a car passing at an offset that clears a live car
      does not clear a wreck. Measured on the drawn frame too: the debris covers
      seventy per cent more ground than the car did.*
- [x] **A landing-prediction arc, off by default.** A dotted trajectory to the
      predicted touchdown while airborne, on **J**.
      *Verification: the car lands **exactly** where the arc said it would — the
      same fixed-point value, not nearly — at a sixth of a gravity, at Earth's,
      and at Jupiter's. It can be that exact because the arc is not a formula: it
      steps a copy of the world forward with the real physics, so an arc that
      disagreed with the race would mean the two were different programs. Asking
      does not move anything, and asking twice gives the same answer.*
- [x] **Decide what surrounds a track.** **A run-off, and then a drop.** Ten
      tiles of sand at the edge's own height, and past that the ground falls away
      steeper than a car can climb. Leaving costs time; carrying on costs the
      race.
      *Verification: a car that brakes on reaching the sand stops inside it and
      is not wrecked; a car that keeps its foot down is finished, at the lip
      rather than after a long slide to the bottom. The surround is drawn, which
      was the actual complaint — the ground used to continue invisibly, so a
      player saw a cliff and drove on a plain that was not there. And the height
      out there is a number: an unbounded drop overflowed Q16.16 for a car thrown
      a few thousand tiles off the map, and returned ground *above* it.*
- [x] **Placing a gate goes into the undo history.** Undo covered terrain,
      surface and gravity but not the route — a promise with a footnote, and the
      footnote was the one edit that changes what a track *is* for every record
      and every shared code keyed on its hash.
      *Verification: place a gate, undo, and the track hash is what it was
      before — the hash and not the count, because a gate put back in the wrong
      place or the wrong order leaves the same number of them and a different
      track. Taking one out of the middle of a route restores its position in
      the order too, and a gate placed inside a brush stroke comes back with the
      stroke.*
- [x] **The store survives a format change.** Both formats moved to version two
      — a record now says when it was set and a profile when it last drove — and
      both readers accept version one and fill the new field with "not
      recorded". A migration path with nothing to migrate from is code nobody has
      ever run, so the format was moved for a reason and the old one kept.
      *Verification: a file in the version one layout, written byte by byte by a
      frozen writer kept in the test rather than produced by the current code —
      which would follow it and prove nothing — loads with every field intact,
      and saving it again writes version two. A version this build has never
      heard of is still refused, in both directions: tolerant of the past, and
      not of the future, because a table of times read by guesswork is worse than
      one that will not open.*
- [x] **Read a foreign track format, and test against a corpus nobody here
      wrote.** `src/core/gs_stunts.c` reads a Stunts (1990) `.trk`: 1802 bytes,
      a 30×30 grid, one plane of road pieces and one of terrain, written from the
      published format. Its three road surfaces — paved, dirt, ice — are the
      three this project started with, so they cross exactly; its two elevations
      become heights; everything else becomes road and is counted as not
      understood, because a car should be able to drive where the donor put a
      road and "it imported" means nothing without knowing how much was
      approximated. **Reading the format is ours and ships; the tracks are
      somebody else's and do not**, per `docs/ASSETS.md` rule 1.
      *Verification: `gearstick_cli import`, in CI, converts a file written in
      the donor's layout by this repository — never a downloaded one — and the
      result validates and is driven by the analyser. Reading the two planes the
      wrong way round, or forgetting that the road plane is stored bottom to top
      while the terrain plane is stored top to bottom, both fail loudly.*
      *What this does not prove: that the reader agrees with a **real** Stunts
      file. A round trip against our own writer shows the two are consistent with
      each other, not that either matches the game. That needs somebody to point
      it at a downloaded track, which is a thing a person does and not a thing
      CI can.*
- [x] **Read the original's designer notes and build stock tracks.** The 1985
      manual's section 7.0 lists all fifty of the original's tracks and says what
      each was for. **The categories are the useful part, not the list**: shapes
      named for their shape, challenges named for what happens on them — `jumps`
      ("big ones"), `headon`, which "aims drivers directly at each other",
      `whichway` with "seven different routes" — test courses after real ones,
      and thirty-odd plain circuits whose point is that nothing is in the way.
      The lesson is that every track had a reason somebody could say in one line.
      *Verification: ten designed tracks ship, each with its reason printed in
      `docs/GUIDE.md` where a player will see it, and **each one raced by the
      analyser before it is written** — a designed track is written out by hand,
      so nothing else checked it, which made hand-built the less verified half of
      the set.*
- [ ] **Sound listened to on Windows and macOS.** The synthesiser is
      platform-independent and the device path is not. **This one cannot be
      finished by whoever writes the code** — it needs a person with speakers on
      each platform. *Verification: a human says it sounds right on all three.*
      **The automatable half is now done and runs on all three.** Opening a
      device, the callback thread the platform runs to pull on it, and the
      stream taking what it is handed are the three things that differ per
      platform, and no test had ever run any of them anywhere: every audio test
      renders the mixer from its own thread with no device at all, deliberately,
      so that the answers are deterministic. A device is opened and fed at the
      end of the run now, on whatever platform the run is on.
      **And the claim underneath it is measured now rather than asserted.** Every
      other sound test compares one thing to another — dirt louder than
      pavement, a mine going off louder than laying one — so a platform whose
      whole output came out at a tenth of the level would have passed all of
      them. One fixed race has its loudness pinned, and MSVC, AppleClang and two
      gccs all land within a tenth of the same number.
      What is left is what the verification actually asks for and no machine can
      give: somebody listening.

## Phase 14 — The network, properly

**Everything in this phase is written down in `docs/THREATS.md` first**, which
says what is being defended, from whom, and what is deliberately not defended at
all. A defence nobody stated is a defence nobody can review.

**Ordered by how much each one matters, not by what depends on what.** The first
four close holes that are open *today* and are cheap; the transport work below
them is larger and defends a channel nobody is currently attacking. That ordering
comes with one thing said out loud rather than discovered: **items one to four are
not complete defences until items five to eight land.** Binding a replay to a
driver stops somebody handing in a recording they found, and it does not stop
somebody claiming to be that driver, because until there are accounts the name on
a datagram is whatever the sender typed. Each is worth doing on its own account
and none of them finishes the job alone.

**Nothing in this phase is invented here.** The pattern is named and specified,
the primitives are audited and widely deployed, and the evidence is conformance
vectors plus a handshake completed against an independent implementation. A
protocol whose only support is its author's confidence is the thing that fails
review, and it fails it for good reasons.

- [x] **A replay says who drove it, and cannot be submitted by anybody else.**
      A recording carries the track, the dials, the grid and the machines, and
      not the driver — so an honest replay is a bearer token, and whoever obtains
      one can submit it as their own with the verifier correctly agreeing the
      time was driven. **The most serious open hole in `docs/THREATS.md`.**
      *Verification: a replay recorded by one profile and submitted by another is
      refused, with a verdict that says why; a name that is a prefix of the real
      one is refused too; a recording that names nobody backs nobody's claim,
      because "it does not say" is not "it says you"; and the same replay handed
      in three times sets one record, not three. A caller asserting no identity
      at all — a local ghost, an offline analysis — still gets an answer about
      the driving.*
      *Still open, and stated in `docs/THREATS.md` rather than implied: this is
      only as good as knowing who sent the datagram, which today is nothing. It
      stops a replay somebody found being spent as their own, and it becomes a
      real defence when the accounts below land.*
- [x] **A submission is bound to the session that asked for it.** Records are
      keyed, so resubmitting the same replay sets one record rather than two —
      but that is a property of the schema rather than a defence, and a thing
      that is safe by accident stops being safe when the schema changes. A
      server-chosen nonce inside the claim makes it deliberate.
      **Sessions live in the database** alongside everything else the server
      knows — a nonce it has issued, who it was issued to, when it expires and
      whether it has been spent. A server that kept them only in memory would
      forget every one of them on restart and would have no way to say whether a
      nonce had already been used.
      *Verification: a claim carrying a nonce the server did not issue, or one it
      issued to somebody else, or one it has already retired, is refused — and
      still refused after the server has been restarted, because the session
      outlived the process that made it. Each refusal is checked against a
      control that does land, so a pass cannot come from the claim failing for
      some other reason. Removing the check turns all six tests red.*
      *Still open, and stated in `docs/THREATS.md` rather than implied: the nonce
      is not from a cryptographic generator and it crosses the wire in clear, so
      it is the shape of the defence rather than the whole of it until the
      transport and the accounts below land.*
- [x] **A race commits to its inputs before it sees anybody else's.** Rollback
      hands every peer the others' inputs for a tick, so a modified client can
      wait and choose. Nothing desyncs, because everybody then simulates the
      dishonest input faithfully — state hashes catch a changed simulation and
      not a changed decision. Each peer sends a hash of its inputs first and the
      inputs afterwards.
      *Verification: a peer that reveals inputs which do not match what it
      committed to is caught and the race stops; a peer that promises two
      different things for one tick is caught too; a peer that shows its inputs
      in the same breath as it promises them gets nowhere, which is the rule the
      whole thing rests on; and a four-player rollback race with the commitment
      in place still agrees tick for tick with the race one machine would have
      run alone. Each of the four turns red on its own when its rule is removed.
      The hash behind the promise is BLAKE2s, checked against the RFC's
      published vector and against an independent implementation.*
      *The cost, stated rather than buried: the reveal runs twelve ticks behind,
      so a remote car's corrections land a tenth of a second later. The local
      car is unaffected.*
- [x] **The whole race is verified, not just the winning lap.** Every peer keeps
      the complete input log and the final state hash everybody agreed on. The
      server re-races the log; a log that does not produce that hash means one of
      the clients was not running this race. Nearly free, because the simulation
      is exactly reproducible — which is the argument for having built it that
      way, collected.
      *Verification: a four-player race over a lossy link produces four logs
      that all re-race to the same ending, and all four are accepted. One
      flipped bit is then refused — and the same flipped bit, with the agreed
      ending taken off the recording, is accepted, which is the size of the hole
      this closes. Removing the check turns the test red.*
      *Said plainly rather than glossed: an alteration that changes no outcome
      is not refused, because it is not a different race — a wrecked car is not
      taking input. The test states the rule that way rather than pretending
      every changed byte is a changed race.*
      *Found on the way and fixed here: an online race recorded no input log at
      all, so a networked time could never have been verified by anybody.*

- [x] **The parsers are fuzzed.** Every byte the server acts on came from
      somebody who may be hostile: the protocol decoder, the chunked
      reassembler, and the track and replay deserialisers behind them. They are
      the part of this program most likely to contain a memory-safety bug and
      they have never been fed anything but well-formed input.
      *Verification: four libFuzzer targets, seeded from captures built by the
      same code the game builds its messages with, run under ASan and UBSan both
      as a fixed-work `ctest` test and as a timed campaign in CI. The
      reassembler bounds its own array index. Around 120 million executions
      found no crash — and a bug planted in the track parser was found in
      seconds, which is how the targets are known to be capable of finding one.*
      *Two things worth saying rather than glossing: the reassembler's index was
      already in range before this, via checks in another file, so that part is
      belt and braces rather than a hole closed. And the carrier's harness was
      nearly useless when first written — planting a bug is what revealed it,
      and it was fixed.*
      *Found and fixed on the way: a chunk the reassembler refused still left
      its declared count behind, so one malformed datagram made that track
      permanently unreceivable.*
- [x] **Nothing on the wire in the clear.** `Noise_IK_25519_ChaChaPoly_BLAKE2s`
      over libsodium as a submodule under `ext/`. A **named pattern from a
      specified, analysed framework** on **somebody else's audited primitives** —
      because a handshake this project invented is the thing that fails a review,
      and deservedly. IK rather than NK: the client already knows the server's
      key, so one round trip gets server authentication, client authentication
      and a client identity a passive observer cannot read. One cipher suite and
      no negotiation, because a protocol that cannot negotiate cannot be talked
      down. See `docs/THREATS.md`.
      *Verification, and it is the verification that makes this reviewable:*
      *(a)* the framework's published test vectors pass, in CI;
      *(b)* **the handshake completes against an independent implementation of
      the same pattern**, which is the one piece of evidence that does not rest
      on our own opinion of our own code;
      *(c)* a captured exchange contains none of the plaintext it carried, and a
      datagram with a single bit changed is refused rather than acted on;
      *(d)* a captured datagram replayed later is refused, while a datagram that
      merely arrives out of order inside the window is accepted — the two are
      different and a naive counter fails the second;
      *(e)* a relayed four-player race still agrees tick for tick with one
      datagram in twenty dropped and the rest reordered;
      *(f)* the handshake and the framing are fuzzed under ASan and UBSan.
      *(a) to (f) all pass under `ctest`. The direct peer mesh is sealed too:
      every peer link is its own IK session, keyed from the static keys the
      broker — the server's lobby or the host's roster — watched each player
      prove. A forged datagram nobody sealed is refused, with a control that the
      real peer's traffic still arrives. The only thing left in the clear is the
      handshake itself, which cannot be encrypted under a key that does not
      exist yet.*
      *Found by (e) and fixed: the last ticks of every race had exactly one
      admissible commitment each, because a flushing datagram reveals the ticks
      it commits. One lost packet and a finished race could never be confirmed.*
      *Found while sealing the mesh, and older than it: an unsealed datagram was
      taken for race traffic, so anybody who knew a port could inject inputs; a
      server race without `--relay` had never sent anything at all; only the
      welcome populated peers, so the first player could not see the second; and
      readiness meant introduced rather than able to race.*
- [x] **A missed checkpoint was silent, and silence cost the race.** Reported
      from play: a corner overshot, then the finish line crossed with nothing
      recognised and no way to tell why. The race tests only the gate a car is
      expecting — which is what stops a lap being won by cutting the infield —
      so one gate driven past stops every later crossing counting, the finish
      included, and nothing said so. The HUD now says "checkpoint missed / go
      back" and an arrow on the ground points from the car to the gate it owes;
      both clear the moment that gate is taken.
      *Verification: a car driven eight tiles wide of a gate three wide latches
      the warning on the right gate, draws the arrow (1,044 pixels against 0
      with it cleared), says so in the HUD (9,308 pixels of difference), and
      clears it on going back. No golden hash moves — the flag is on the view,
      not in the world, because having been told is a property of a screen.*
      **Ticked here before it had ever reached a screen, and that is the part
      worth keeping.** The flag was set every tick and destroyed every frame:
      the client rebuilt its views from a blank array each frame and copied the
      switches back one field at a time by name, so a field added without a line
      added with it was wiped. Every test above passed, because they drove the
      HUD and the arrow directly and none of them crossed that seam. The
      splitter now sets only the car and the rectangle it owns and the client
      hands it the views it already has, so adding a field to a view is safe by
      default rather than safe if somebody remembers. The checkpoints are on the
      minimap with it — every gate a dot, the one owed ringed in white, orange
      once it is behind you — and the HUD row now reads "checkpoint / GO BACK",
      because "checkpoint missed" did not fit the panel and was drawn as
      "checkpoint misse".
      *Verification: a view filled with values nothing else produces is put
      through the splitter at every number of players on both sides of the merge
      — 13 views over 8 arrangements, 6 merged and 2 split, with both paths
      required — and demanded back **whole** rather than field by field, so the
      next field somebody adds is covered without editing the test. Putting the
      bug back turns it red.*
- [x] **The cars are half as quick again.** Asked for after driving the shipped
      set: the game felt sluggish against the one it is after. Power, top speed
      and grip are each half as much again on all six machines, and toughness
      with them — because half as much speed again into a jump is more than
      twice the damage out of it. Braking, steering and drag are untouched, so
      a corner asks for more precision than it used to rather than less.
      *Verification: a lap of the pace circuit goes from 27.27s to 20.43s, a
      quarter quicker, which was what was asked for. All three dials were needed
      — grip alone buys 5% and power with top speed but no grip buys 13%.
      Four AI cars raced a lap of each of the eighteen shipped tracks: without
      the toughness 15 of the 72 are wrecked, with it 7 — and that 7 is what the
      same grid was already losing before any of this — the open item below on
      choosing a track by racing one car, not something this introduced. Every machine is
      still best at something, in the same spread of wins it had before. The
      golden world hash moves, deliberately, and says why.*
      Nine tests moved with it, and none of them because the product broke:
      their fixtures had old speeds written into them — a 40×16 field, a 64-tile
      strip, "five tiles a second", a twenty-second derby — and faster cars ran
      off the end of the measuring stick. Where the rule could be stated instead
      of the number it now is: the cornering test finds the speed each driver
      gives up at rather than asserting what they do at five tiles a second, and
      the editor's build-and-race loop is driven by the game's own AI rather than
      by a hard-right circle that used to happen to pass through both gates.
- [x] **Every generated circuit had a corner nobody could take, right before the
      start line.** Reported from play as "the strange thing before the start
      line ... it is unnavigatable". The closing arc was centred on the point the
      route starts from, and an arc of radius r about a point ends r away from
      it — so the route stopped short of its own beginning and the lap wrapped
      across the gap, putting a 157° reversal at about a tile's radius into every
      loop the generator has ever made. It closes with a single half circle now,
      the same turn the serpentine uses everywhere else.
      *Verification: the sharpest turn between any three consecutive gates on a
      generated route is now 46.8°, against the 157° reversal it replaced. The
      suite walks 2,073 corners over every shape the generator makes — 6 loops
      and 18 paths — and fails on anything over 90°, so it is the rule that is
      pinned and not the six shapes that exist today. The generator hash moves,
      deliberately, and says why.*
- [x] **The shipped set is rebuilt on the fixed generator.** The eighteen are 6
      circuits and 12 sprints, and the six files that changed are exactly the
      six circuits — the closure is a loop-only construct, so a sprint's ground
      is untouched. "Circuit" cuts across "written by hand": the authored tracks
      lay their routes with the same planner, so `the oval`, `the crossing` and
      `the long way round` moved with the generated ones. Ten of the twelve
      sprints are byte for byte what they were; the other two changed because a
      track ships only if all six machines can finish it and the machines got
      quicker in the same breath, so `first ridge` and `low bend` are in and
      `grey ridge` and `wide flats` are out. Those two had to be *deleted* — the
      baker writes the set but has never removed from it, so both were still on
      disk and still tracked.
      *Verification: the eighteen files in the box are byte for byte what the
      baker writes today, checked by baking into an empty directory and
      comparing all eighteen; every one is validated and raced, and the
      route-length floor still holds.*
- [x] **The way back to a checkpoint was drawn in pieces.** Reported from play:
      "parts of it are visible and then not visible, it is like the image is
      oscillating." Ground paint is cut into half-tile pieces and drawn one
      depth at a time by the terrain sweep, which is what stops a mark painting
      over a car standing on it. This arrow is drawn *after* the sweep, as a
      readout, and passed its own shape's depth — so only the few pieces sharing
      it were drawn, and a different few qualified each time the car moved a
      tile. Five call sites in the same file already did it correctly and the
      helper's own comment described this exact flicker being fixed once
      before; this was the wrong argument with the right one demonstrated
      alongside it. There are two entry points now, one for the sweep and one
      for a readout, so there is no depth to get wrong.
      *Verification: sixteen alignments of the car across two tiles, counting
      the arrow's pixels at each — 1099 to 4617 with the bug, 5437 to 5583
      without, and the test requires them within a fifth of each other. The
      test that was already there asked only for "some orange" and passed
      throughout. Putting the bug back also fails to compile, because the
      readout entry point goes unused.*
- [x] **The HUD says how fast, and which way.** A bar rather than a number,
      because a figure that changes every frame is read by nobody at speed.
      Zero sits a quarter of the way along and it grows both ways on one scale,
      so reverse is the same ruler backwards: full forward is the machine's own
      top speed, full reverse a third of it. Forward in the accent colour,
      reverse in the warning orange, and it reads the speed along the way the
      car is pointing, so a car sliding backwards down a slope says so.
      *Verification: drawn in all twenty-four states the panel has, none of
      which hides anything.*
      Two faults in the panel's arithmetic came out of adding the row, one of
      them nine pixels old: the carrying row had been charged a gap short since
      weapons landed, fitting only because there was slack above to absorb it,
      and the zoom search left up to a whole step of empty panel because it
      stopped at the first size that fits. The test's allowance is a line of
      text now rather than the number twelve — twelve *was* this font's line
      height, and a missing row, which is what the check exists for, is thirty
      pixels and more.
- [x] **Reverse was the fastest gear in the game.** Found while deciding what
      the speed bar's scale should be. Forward thrust falls to nothing at the
      vehicle's top speed; the reverse branch had no such rolloff and simply
      accelerated until drag balanced it, so every machine reversed two to three
      times faster than it could drive forwards — the stock car 23.5 against a
      top of 9.0, the motorcycle 30.1 against 9.9. Reverse now tops out at half
      the forward top, because reverse thrust is already half the power: the
      same halving in both places rather than a second dial to tune per machine.
      *Verification: every machine on every ground at four gravities — 216
      combinations — none exceeding half its own forward top, and every machine
      still reaching that half on pavement, so a cap that stopped the car
      moving would not pass. Putting the old branch back turns it red, naming
      the machine and the ground. No golden hash moves, which is itself worth
      knowing: neither pinned race ever reverses, so nothing recorded was
      testing this at all.*
- [x] **Back went to the main menu from a race you were standing in.**
      Reported from play. Escape out of a race lands on the race setup, and Back
      from there abandoned the race — which was still sitting in memory, paused,
      with no way to return to it. The tracks list did the same to a grid you
      were halfway through filling in. A screen reachable from two places has to
      know which one it came from; the records table already worked that way and
      these two never got it. Paused, the row now reads **NEW RACE · Resume ·
      Main menu**, and Escape resumes rather than dropping you on the title.
      *Verification: the way out of the setup screen is walked from all nine
      origins, on a server and off one, and the tracks list from all nine — the
      treatment the records table already had. The panel walk gained a starting
      state with a race behind it, went red by itself when it could not reach
      the new buttons, and now reaches 52 of the 52 controls the menu names.*
      **The test that should have caught this had written the fault down as the
      expected answer**: it asserted Back from setup goes to the title without
      ever asking where setup was reached from. And the panel walk could not
      have caught it at all — it checks that controls are reachable and inside
      their boxes, and a button that is reachable, correctly sized and wrong
      passes every assertion in it. Exhaustive over geometry is not exhaustive.
      Left uncovered and said so: that the frontend *resumes* rather than
      restarts lives in the iterate loop rather than in anything callable, and
      is checked by hand.
- [x] **Every track was the same shape.** Reported from play with a picture:
      "every track can't be the same shape — there needs to be combination of
      path and circuit tracks, with different paths, different terrain". There
      have been four track shapes since the generator was written and **not one
      of them ever reached the route planner** — they choose terrain and
      surfaces, and the route was always the same serpentine of horizontal
      passes, for hand-written tracks as well, because those use the same
      planner. The seed draws a layout now: the serpentine as it was, or the
      same turned a quarter so its passes run north and south.
      *Verification: the set rebuilt from it runs 55 to 92 gates and 131 to 250
      second laps, where every track used to be 83 to 92 gates and 188 to 260
      seconds. The hairpin rule still holds at 47° over 1,727 corners, every
      track is raced by every vehicle from every grid slot, none throws a car
      off the world, and the length floor still holds. The generator hash moves
      and says why.*
      **The cause was the floor, and it is arithmetic rather than an oversight.**
      The longest closed curve that fits a field under two hundred tiles across
      is four hundred tiles; the floor is six hundred and thirty. A circuit could
      therefore only ever meet it by folding, and anything folded six times in a
      square field is a serpentine. Adding lobes to a loop makes it *shorter*,
      because the base radius must shrink to keep them inside, while its tightest
      turn collapses from 64 tiles to 5 — both measured before any code changed.
      What the floor exists for — a race that is not over in twenty-seven
      seconds — is untouched. What it was accidentally dictating was the shape.
      **A circuit is driven several times, so the floor is now a floor on the
      race**: route times laps, in one place, used by the tool that writes tracks
      and the suite that checks them. Paths are driven once and still fold.
      Circuits are closed curves now — 30 to 35 gates and 75 to 93 second laps,
      driven three times — where paths run 55 to 87 gates and 131 to 245 seconds
      end to end. Two kinds of track that are different kinds of thing.
      **Still outstanding from the same report:** the jumps band — none, small,
      big — as a dial of its own, with gravity and surface varying inside it.
- [x] **The dialog you opened over a race ignored your first click.** Reported
      from play, and the second report was the diagnosis: "it doesn't have focus
      — I click on the dialog first". A menu opened over a race arrives behind
      the race's own windows and ImGui leaves the focus where it was, so the
      first click was spent taking the focus rather than pressing what it landed
      on. The frontend asks for the focus at the one moment that happens.
      *Verification: all eight screens that draw a panel are arrived at from
      another screen and must be the window taking input on the frame they
      appear. The walk is unchanged at 812 of 812 controls — the number that
      says the fix cost nothing.*
      The obvious rule, "focus whenever the screen changes", took the focus off
      the dropdown the front-end walk had just opened and cut it from 812
      controls to 289. Nothing could have caught the original fault: the walk
      presses through ImGui's test engine, which sets focus itself.
- [x] **Delete asks before it throws a track away.** It is the one thing on the
      tracks screen that cannot be undone — the library is the only copy of
      somebody's own work and there is no bin — and it went through on a single
      click, sitting between two buttons that are harmless. It names the track,
      because "are you sure?" without a name is a question nobody can answer
      safely, and the screen behind it goes inert, which is what a modal means.
      *Verification: the walk reaches all 54 controls the menu names, including
      the two the question draws, from a starting state with a question up; the
      panel fits the window in that state as in every other; and the walk is
      whole at 814 of 814 controls.*
      **It took six attempts, and the five that failed are why the walk needed
      changing.** A dialog is a menu state, and the walk tells states apart by
      hashing them: as the hash of the track asked about it was 32 more states
      a screen and the walk never finished; as a bool it was one more and the
      walk went from four minutes to fifteen; kept out of the hash the walk
      could not see the dialog at all and pruned the screen behind it, 812
      controls becoming 289. Making the screen inert underneath cost only six
      per cent more states — the right shape — and then ran the walk out of its
      **pending-state queue**, and after that out of its **path depth**, neither
      of whose margins had ever been measured. Both are raised, and both say why.
      The last of it was not the walk at all: the question was being added to
      the bottom of a screen that was already the tightest fit in the game. The
      detail panel stands down while the question is up — it describes the track
      the question already names — and that is what made it fit.
- [ ] **The transport has a written specification.** Message formats, the state
      machine, the key schedule, the message limit before a rekey, and the
      properties claimed **and explicitly not claimed**. A design nobody wrote
      down cannot be reviewed, and the parts left out are what a reviewer most
      needs to see were decisions.
      *Verification: somebody who has not read the code can implement a client
      from the document alone and complete a handshake.*
      **The document is written — `docs/TRANSPORT.md` — and this stays unticked
      because the verification needs a reader, and the person who wrote the code
      cannot be that reader.** It gives byte offsets for both handshake messages
      and the sealed envelope, the key schedule, the nonce and replay-window
      rules, the message limit and the absence of a rekey, and a section of
      things it explicitly does not claim: no denial-of-service resistance, no
      post-compromise security, no traffic-analysis resistance, no formal proof.
      The byte sizes it quotes are pinned by a test, so the document cannot
      drift from the code without something going red. What is missing is
      somebody implementing a client from it who has not seen `gs_noise.c`.
      **How this will be closed:** a fresh agent given the document and
      nothing else - no source, no tests - writes a client from it and runs
      a handshake against the real server. If it completes, the document
      was enough; if it stalls, where it stalled is what the document is
      missing. Scheduled between items 3 and 4 above.
      **The check that can be automated now runs rather than skipping**: on
      2026-08-27 the `noiseprotocol` package was installed, and
      `gearstick_transport_document` writes a client from the document alone,
      completes a handshake with a real server, and confirms every byte size the
      document quotes — 96 for the first message, 48 for the second, 38 for a
      sealed datagram carrying eight bytes. Both messages, the prologue, the key
      schedule and the framing are therefore known to be right *as written*. It
      remains not the verification, for the reason above.
- [x] **A profile you can prove is yours.** A password, and a second factor for
      anybody who wants one. **This comes after the tunnel and is small because
      of it**: inside a sealed channel a password can simply be sent, and a
      one-time code is the arithmetic it should always have been. Before the
      tunnel it would have needed a challenge-response construction to avoid
      sending either in the clear.
      *Verification: a name with a password is not taken by typing it and not
      taken by the wrong password, and is taken by the right one; a one-time
      code that has been used once does not work a second time inside the window
      it is still valid for; a name with no password still works by typing it;
      and a claimed name can only be re-passworded by whoever proved it. Each
      turns red on its own when its rule is removed.*
      *The code is TOTP-SHA256 rather than SHA-1 — RFC 6238 names all three and
      libsodium ships the two it is not — checked against the RFC's own
      published values and against Python's `hmac`. Said here because a phone
      app defaulting to SHA-1 will give six wrong digits.*
- [x] **A track has an owner, and the ones that shipped have none.** Whoever
      built a track can change it, take it down, keep it private, hand it to a
      named few, or publish it to everybody. The stock tracks are outside all of
      it and no request can touch them.
      *Verification: two clients both calling themselves ada, only one of whom
      built the track — and only that one can take it down; a track handed to
      one named person is listed for them and for nobody else, including
      somebody asserting no identity at all; and every write path aimed at a
      shipped track is refused, including by a profile called the same thing as
      its author. Each of those turns red on its own when its rule is removed.*
      *Said plainly: there is no delete over the wire. The store's delete is
      owner-checked and tested, and no protocol message reaches it — so "cannot
      delete somebody else's" holds by there being no delete at all.*
      *Ownership is a key rather than a name, which is a check that could not be
      written before the tunnel: the server had nothing but what somebody typed.*

## Phase 15 — Installers

The releases are archives you unpack. On Linux that is normal and on macOS the
disk image is the convention, but on Windows an application people actually
install is an installer.

- [x] **An MSI for Windows, built by CI.** Installs the game, the headless
      driver and the assets, puts it in the Start Menu, and uninstalls cleanly.
      *Verification: the MSI installs on a machine that has never had a
      toolchain on it, the game runs from the Start Menu, and uninstalling
      leaves nothing behind.*
      **Verified, including the machine.** CPack's WiX generator makes it
      alongside the zip, with a fixed upgrade GUID so a new version replaces the
      old one rather than installing beside it.
      *On the runner: installed with `msiexec /qn`, the install location found
      rather than assumed, the assets checked, the golden replay re-raced from
      the installed copy, the Start Menu shortcut resolved and **launched**, and
      then uninstalled with neither the program nor the shortcut left behind.*
      *And on a machine that has never had a toolchain: a Windows Server Core
      container, which refuses to proceed unless it first confirms there is no
      compiler, no CMake and no Visual C++ redistributable on it. The MSI
      installs there and the golden replay re-races.*
      *That last part was only possible because of what it turned up on the way:
      MSVC links the dynamic C runtime by default, so the game needed the
      redistributable — invisible on every machine that had ever tested it,
      fatal on a player's, and unfixable by a zip. The runtime is static now and
      `dumpbin` checks the installed executable imports none.*

- [x] **The installer is in the release beside the archive.** Somebody who wants
      a zip still gets a zip. *Verification: a tagged release carries both, and
      both carry a provenance attestation.*
      *Verification: `v0.1.0-beta1` is published and carries all five files — a
      Linux tarball, a macOS disk image and tarball, a Windows zip and an MSI —
      with one `SHA256SUMS` over the lot. `gh attestation verify` accepts the
      tarball and the MSI against this repository, so the provenance is
      something somebody can check rather than something the workflow claims.
      It is flagged as a pre-release: semantic versioning says a hyphen means
      one, and a beta should not be handed to somebody who asked for the latest
      release.*
      *Two things had to be fixed before a tag could produce a correct release,
      and both were found by being told to cut one. `GEARSTICK_RELEASE` was
      documented as "set by the release job from the tag" and no job set it, so
      every file would have been named `0.1.0` with the beta marker gone. And
      Windows Installer refuses a product version that is not four integers, so
      the MSI carries `0.1.0.1` while the file keeps the name a person reads —
      the fourth field coming from the pre-release number, which is what makes
      beta2 install over beta1 as an upgrade instead of beside it.*

## Phase 16 — The front door

The game had a front end and no way to say who you were. It opened on a title
screen anybody could walk past, and a client pointed at a server skipped the
front end entirely and drove straight onto the grid — so the one situation
where it matters most whose lap time this is was the one situation that never
asked.

- [x] **Every driver has a password**, and a six-digit code from a phone for
      anybody who wants one on top. A driver carrying no password cannot sign in
      at all — a roster from before passwords existed is not turned away, it is
      offered one on the way in.
      *Verification: a roster saved before passwords existed still loads, the
      right password gets in, and a wrong one, an empty one and a nearly-right
      one do not — nor does a right code with a wrong password.*
- [x] **Nothing is reachable until somebody signs in.** The game opens on the
      door. The title screen, the track library, the settings and the records
      are all behind it, and the check is made in one place rather than by each
      screen remembering to make it.
      *Verification: defeating the password check makes the test fail, which is
      how we know the test is testing it.*
- [x] **The menu is play, tracks, profile and exit.** Play starts by choosing a
      track and goes on to the settings, the way the 1985 game did. Tracks is
      for looking after the library, Profile is for the driver signed in
      including setting or removing their password, and Exit leaves.
      *Verification: the screens are drawn and photographed by `--screen`.*
- [x] **A client pointed at a server shows the front end.** It signs in first,
      instead of joining the race as whoever the command line said.
      *Verification: a client launched at a running server sits at the login
      screen rather than reporting that it is driving.*
- [x] **Signing in lands on the menu, and only the lobby joins a race.** Play is
      what puts an online player in the queue; until then they can look at the
      records or build something with the race waiting.
      *Verification: a client left on the menu with a server ready and waiting
      does not start racing, and one left in the lobby does — both checked
      against a real server, and the check fails if the rule is removed.*

## Phase 17 — The front end, walked by machine

Every navigation fault this week was found by somebody sitting and clicking: the
results screen that put itself back, the Race button that did nothing, the
editor you could not get out of. The front end is the last part of the game
still checked by hand, and a person only walks the paths they think of.

A walk exists and it is one move deep: from a fresh menu placed on each screen
it presses Tab *n* times, presses Space, and writes down where that lands. It
would have caught this week's faults and it is worth keeping. **It is not
coverage of the paths through the front end and is not to be described as one.**
What is wanted is a walker that signs in for itself, presses on from where it
got to, and keeps going until it has seen every state the front end can be in —
and the front end includes the construction set, which is where nearly all of
the combinations are.

**The standard here is every scenario, not a representative sample of them.**
Where a sample has been taken so far the game has been shipped with faults a
person then found by clicking, which is the whole reason this phase exists. So
no item below bounds what is covered in order to fit a budget. The budget is
what gets fixed: a step that costs eight milliseconds because it rasterises a
1280x720 frame is what makes exhaustive look unaffordable, and that step does
not need to draw anything at all. Make the step cheap, then walk everything.

- [x] **A frame that is drawn is presented.** SDL hands back the draw commands
      and vertices it has queued only when the renderer is presented or flushed,
      so a test that drew forty thousand frames and presented none of them grew
      without bound — and took the machine down rather than failing.
      *Verification: the render suite now holds flat at about 450 MB where it
      used to climb past 2 GB, and every renderer test passes.*
- [x] **A test with a runaway in it fails instead of taking the machine.**
      Sanitized tests run under a two-gigabyte ceiling.
      *Verification: the same test, before its fix, stops itself and names the
      limit it hit, instead of ending the session and leaving no log.*
- [x] **A step of the walk stops costing a drawn frame.** Every keypress was
      rasterising two full frames through the software renderer under
      sanitizers, and a walk that only lays a menu out and reads it back needs
      no pixels at all. The walk's frame now stops at the layout.
      *Verification: the renderer suite went from seven minutes forty to twelve
      seconds and says exactly what it said before — the same tests, all
      passing. It fits its three-minute budget with room to spare, where before
      it did not finish at all.*
- [x] **A destination that is not a screen fails the test.** The walk's
      description had claimed this all along while the code quietly dropped such
      a destination — the same condition that filtered out a screen leading to
      itself was swallowing it.
      *Verification: a button rigged to name a screen that does not exist turns
      the test red and names it; putting the button back turns it green. That is
      how we know the check is checking.*
- [x] **Controls are known by name, not by how many Tabs away they are.** Dear
      ImGui already reports every item it draws and can be told to press one by
      name; those hooks are now wired up, so a screen can be asked what is on it
      — including the controls drawn dead and the ones the keyboard skips, which
      a Tab walk can never see at all.
      *Verification: a button inserted at the very top of the title screen added
      itself to the map and left all 190 other controls with the names they
      already had. Pressing by name reaches every screen that pressing Tab
      reached, and dropping the names turns the test red.*
- [x] **A menu state can be recognised when it comes round again.** `gs_menu_hash`
      is one number for the state a menu is in, and what it leaves out is the
      design: the borrowed view of the lobby, the panel measurement taken while
      drawing, and the two values that advance with the clock rather than with
      anything anybody pressed — hashing those would make every frame somewhere
      new, which is the same as having no hash at all. A lobby error counts by
      its message and not by where the message is stored.
      *Verification: all 52 fields are classified in the test, not a sample of
      them, and each is proved by flipping a byte in it — the ones that are
      state must move the number and the four that are not must leave it alone.
      Dropping fields from the hash names them; adding the clocks to it names
      those. A copy of a menu is the same state, which is the move a walk makes
      on every step.*
- [x] **The walk goes as deep as the front end does.** Breadth-first from the
      states already reached rather than one press from a fresh menu. Two
      corrections were needed on the way and both are worth knowing: walking the
      menu's *bytes* does not terminate and cannot, and walking whatever looks
      new does not either. A state is now what a screen is **offering** — the
      controls on it and whether each is live — and the reason to explore one is
      that it offers a control nobody has pressed.
      *Verification: **694 of 694 pressable controls pressed**, asserted rather
      than reported, over 102 distinct offerings and 4,111 presses in 41
      seconds. The eleven controls that can never be pressed are counted apart
      so the number means what it says.*
- [x] **Every way in, not just Tab and Space.** Escape, the arrow keys and typed
      text are all things a walk can do now. The door turned out to want a name
      *and* a password typed, not a driver picked off a list, which is the sort
      of thing only a walk that has to open it for itself finds out.
      *Verification: seeded signed out at the login screen with no screen handed
      to it, the walk types the driver's name, types a password that happens to
      be the right one, and arrives at the title — in two moves, with a wrong
      password in its vocabulary too.*
- [x] **The conditions the buttons are under.** The walk now starts from seven
      menus rather than one — everything, signed out, offline, an empty library,
      no track picked, no results yet, alone in the lobby — sharing one set of
      books so each is asked only what it can reach that the others could not.
      **The answer is smaller than expected and is the interesting part**: six
      of the seven add nothing, because the walk can press its way into most of
      those conditions by itself. The one that pays is *offline*, which is a
      thing no button can change.
      *Verification: 730 of 730 pressable controls pressed, of which **3 are
      reachable only by seeding** — asserted, not reported, so a day when
      seeding stops mattering is a day the tree goes red and somebody looks at
      why.*
- [x] **The lap dial, one to twenty, actually raced.** Every value the slider
      offers, each one driven to its finish. The car is held on full lock at a
      steady speed rather than driven by the racing AI, because what is under
      test is the lap rule and not the AI — which laps a bare rectangle twice
      and then sits in the run-off, reading exactly like a lap counter stuck at
      ten.
      *Verification: all twenty finish on the lap they were asked for, and the
      crossings come to one more than the laps every time — which is the run-up
      to the line, and the off-by-one that would end a three-lap race after
      two.*
- [x] **The rest of the setup screen's dials reach the race.** Every mode, every
      player count the grid has room for, every vehicle.
      *Verification: 12 values, against a race built from them — each car on its
      own grid slot with no two in the same place, and each vehicle arriving as
      the one that was chosen and driving under its own power.*
- [x] **Every value of every dial, not three interesting ones.** Laps one to
      twenty, both modes, every player count, every planet — in the race setup
      *and* in the construction set's palette, which are now one list of planets
      instead of two that happened to agree — and every driver, machine and
      colour on **every row of the grid**. Pressed on the screen, and each from
      a state it was not already in, because a button that is already the one
      chosen does nothing and so does a dead one.
      *Verification: **106 of 106 values**, counted out of the game's own
      numbers rather than a list in the test. Hiding one machine from the screen
      names it missing on all four rows and turns the tree red.*
- [x] **The number of players changes what the setup screen *is*, so the walk
      changes with it.** Seeded at one, two, three and four players, and with a
      guest in a seat — a guest is not a roster driver and does not draw the
      same row.
      *Verification: **750 of 750** now, up from 730. Three players draws ten
      controls no other starting state does and four players draws ten more, so
      the setup screen at four is a different screen from the one at two rather
      than the same screen with more rows. Controls reachable **only** by
      seeding went from 3 to 23.*
- [x] **What the walk proves, said as properties.** Three of the four, asserted
      against the graph the walk builds as it goes: **no screen is a trap**,
      **the title is reachable from everywhere**, and **everywhere is reachable
      from the title** — which is the other direction and a different claim, and
      the only half that catches a screen nobody can get *to*.
      *Verification: 0 traps, 0 stranded, 0 unreachable, over the eight screens
      the walk stands on. Two are named rather than counted: the sign-in door,
      which is left by typing a password and is proved by the door test next to
      it, and the results screen, which is arrived at by finishing a race and
      which no button leads to — correctly.*
- [x] **No control does nothing everywhere.** Every control changed something,
      somewhere. Ten looked dead and nine of them were the walk's fault rather
      than the front end's — a preset already chosen, a row already picked, a
      slider that is moved rather than pressed, a box typed with what was
      already in it — so a control that did nothing is now taken back to where
      it did nothing and tried against **every other control on that screen**,
      with the arrow keys, and with every word the walk knows.
      *Verification: **0 of 569 controls do nothing everywhere**, with 9 woken
      by being retried properly, in 256 extra presses. One is excused by name
      and it is the share code, a read-only box you copy a track out of — and
      the excuse is itself asserted, so the day it can be typed into, the tree
      goes red rather than quietly covering for whatever goes dead next. A
      button rigged to do nothing is named and turns the tree red.*
- [x] **The walk can scroll.** A person reaches the bottom of a long list with
      the wheel, so the walk does: it winds a table to the top, walks down a
      tick at a time, and presses everything that comes into view — which is how
      the twenty-one tracks below the fold of a full library got pressed for the
      first time.
      *Verification: **0 controls drawn and out of reach**, down from 86, and
      the count of controls covered comes back up from 663 to 749. Stopping the
      wheel from working puts all 86 back and turns the tree red.*
- [x] **The editor is walked by machine as well.** Nothing had ever pressed a
      button in it: `gs_editor_frame` was called in exactly one place in the
      whole repository and that place was `main.c`, so every control in the
      construction set had only ever been checked by somebody clicking it. The
      walk now drives the real palette.
      *Verification: **56 of 56 named controls pressed**, across 46
      configurations of the palette — every brush, every surface under the
      surface brush, every piece in the parts box, the panels open and shut, and
      a route with gates on it so the buttons that remove them are drawn. What
      is counted is what *moved*: an activation that lands on the floor is not
      coverage. ImGui's own furniture — title bars, resize grips, its implicit
      debug window — is named as such and counted apart rather than folded into
      the total.*
- [x] **Every brush and every option it carries does what it says.** Not
      pressed once with whatever it was last set to — every value of every
      option, against the panel's own ranges: radius 0 to 8, step 0.05 to 2,
      gravity 0 to 3.9, gate heading 0 to 359, half width 0.5 to 8, every
      surface, every piece in the parts box, and all four dials.
      *Verification: **5,563 option values checked**, each against what it did
      to the track. The continuous ones are walked at a hundredth, finer than
      the panel shows and finer than a mouse can land on. Heights are checked to
      the 256th of a tile the track stores them in, and the four dials are
      checked through `gs_editor_apply_dials` — including that the gravity dial
      goes through the multiple-of-Earth conversion rather than round it, which
      is the fault that once made every race run at forty percent of what it
      claimed.*
- [x] **Brushes are walked in combination, exhaustively.** Ice on a slope,
      gravity under a ramp, a gate on ground that moves afterwards. Two
      properties, and between them they say a brush is *for* one thing: what it
      is not for it leaves exactly as it found it, and what it is for it does
      the same regardless of what was there before.
      *Verification: **147 brush-and-setting configurations, all 21,609 ordered
      pairs of them walked** — the count is stated and asserted equal to the
      space, so a brush added later and left out of the sweep turns the tree red
      by itself.*
- [x] **Building a track is walked end to end, by pressing and dragging.**
      Press New, choose each brush off the palette, hold the mouse down and drag
      a ridge, pick ice out of the surface list, wind the gravity brush down,
      click a route, keep the result in the library and type a name for it —
      then race what came back out of the library. There was already a test that
      did all of this by calling functions; a brush unreachable from the palette
      passed it.
      *Verification: the track built by hand validates, goes into the library
      under the name that was typed, comes back out hash for hash, and is won.
      **Two real layout faults fell out of it**: the box under THIS ONE took the
      whole panel when nothing was chosen, and the setup panel was a fixed
      height at every player count, so at four drivers seventy-two pixels of it
      — including the Race button — were below the bottom edge of a window
      nobody can move or resize.*
- [x] **Undo is walked against the whole build, not against single edits.**
      The ridge, the ice over it, the gravity over that and the route — taken
      back in order and put back again.
      *Verification: **every prefix**, not just the ends. Each undo is written
      down and each redo has to reproduce that exact state in reverse, so an
      editor that got the middle wrong in a way that cancelled out still fails.
      All the way back is a blank field with no gates; all the way forward
      hashes identical to what was built.*
- [x] **The walk counts what it covered and fails if anything was missed.**
      The number is no longer the walk's own: the screens name their controls in
      the files that draw them, and that text does not care what any walk
      reached. Every label `gs_menu.c` and `gs_editor.c` write down has to have
      been met.
      *Verification: **46 of 46** in the front end and **30 of 30** in the
      construction set. A button added to a screen nobody can reach is named and
      turns the tree red, with no case added for it — checked by adding one.
      Turning it on found **fourteen controls neither walk had ever met**: four
      needed new starting states (a lobby ready to race, a driver with no
      password, a server asking for a code, a track that came with the game),
      three were inside combo boxes nothing ever opened, five were below the
      fold of the editor's own panels, and two were the same control wearing two
      names.*

## Phase 18 — Opponents worth racing

There is a driver in the game and no way to race it. `gs_ai_drive` plans a line
from the grip it has at that moment rather than following a baked one — which is
why it re-thinks a corner when somebody moves the gravity dial — and it has been
finished and tested since Phase 8. But every car in a race takes its input from
a pad: the AI drives only in headless self-play, the editor's background ghost
and the demo. **A one-player race is one car going round on its own.**

- [x] **A demolition derby has a demolition derby's HUD.** *(Found by looking at
      one, which nobody had done.)* Four of its five rows were about getting
      round a track — position, lap, this lap, best — and in "last one driving"
      none of that decides anything; the one question the mode asks, how many
      are left, was not on the screen at all. It read `position 1/4` with two of
      the four already wrecked.
      *Verification: the HUD test renders twelve states rather than seven — both
      modes, wrecked, finished, waiting, counting — and asks two things of each:
      what fell off the bottom, and how much of the panel was nothing. "Still
      driving" has one definition, shared with the rule that ends the race.*
- [x] **The first screen a new player sees offers the thing that can work.**
      *(Found by running with an empty preferences directory — nobody had ever
      looked at a fresh install, because this machine has had a driver on it
      since the door was built.)* SIGN IN was the big blue button and cannot
      succeed on a machine with no drivers on it; what it says when it fails
      reads like the game refusing somebody who has done nothing wrong.
      *Verification: an empty roster draws one sentence and one loud button, and
      the walk starts from that state too — seventeen seed menus now, because
      `NEW DRIVER` is a label no other starting state produces and the count
      taken from the source insists every label is reached.*
- [x] **No test writes where a player keeps their things.** *(Found by
      photographing a race and noticing the preferences directory had been
      written a minute earlier.)* The editor saves the current track and the
      chosen bindings there, and the walk presses every control in it — so the
      day the walk learned to wind a panel down far enough to reach `save`,
      running the tests began overwriting a real player's work.
      *Verification: every test runs with its preferences pointed at a throwaway
      in the build tree, and the render suite checks that it is: point it
      anywhere else and the tree goes red.*
- [x] **Race position is progress along the route, not distance to a point.**
      *(Found by photographing a four-car race: the HUD told the player on pole,
      before anybody had moved, that they were third of four.)* A gate is a line
      across the road and a car crosses it wherever it likes, so measuring the
      straight line to the gate's centre made the car in the middle of the road
      lead the car level with it on the outside.
      *Verification: four cars abreast on a standing grid are first, second,
      third and fourth in that order; moving one along the road changes the
      order and moving one across it changes nothing. Every earlier test of this
      rule spread its cars along the track, which is why none of them saw it.*
- [x] **A tool panel that hides something is using the whole screen first.**
      *(Found by photographing the construction set, which had never been looked
      at in a picture.)* The palette ended at the route check with save, load,
      the code buttons and the line telling a new player what the mouse does all
      below the fold — under a window with a quarter of the screen empty beneath
      it. The route list scrolls in a box of its own now, the way the library
      already does.
      *Verification: asserted in all 46 configurations of the editor walk. Not
      "nothing scrolls", which would be a lie for a list of any length — a panel
      may only hide something once it is as tall as the window allows.*
- [x] **Nothing is drawn past the edge of its panel.** *(Found by photographing
      the setup screen after the skill dial went in.)* A panel can sit inside
      the window, scroll nowhere, and still draw the last button on a row cut in
      half — and no item hook can see it, because what they are handed has
      already been clipped.
      *Verification: every screen from every seed reports nothing wider than its
      own panel. Taking the dial out reports nothing; putting it back reports
      thirty-three pixels in every state, which is Venus and Jupiter ending in
      the middle of their own names.*
- [x] **The empty grid slots can be filled with opponents.** A seat on the grid
      is the game's until somebody takes it: the driver list on the setup screen
      offers *computer* alongside the guest and the roster, and it is what an
      empty slot starts as.
      *Verification: a race for four with nobody at the keyboard finishes with
      three cars timed and an opponent winning it. What the setup screen means
      is now a rule with a test over it rather than a paragraph in the client,
      and the game's own cars stay out of the records — a table with the
      computer at the top is a table nobody can get on.*
- [x] **Their skill is a dial, not three names.** Twenty-one settings from a
      driver who brakes far too early to one who is quicker than you are. The
      three names that used to be the whole of it are now three points on it.
      *Verification: **84 lap times, every step strictly quicker than the one
      below it, with no ties** — in four sets of conditions, because the driver
      is not tuned per track: pavement, dirt with two thirds of the grip, the
      Moon with a sixth of the weight, and a different machine. Every setting
      gets round; a timid driver is still a driver.*
- [x] **The dial changes how they drive, not just how fast.** It moves three
      things together, because they are the same confidence: how much of the
      grip they ask for, **where they lift**, and how straight they hold it.
      *Verification: the two closest settings on the whole dial lap 25 ticks
      apart over three laps and still leave the same ramp at different speeds
      and land in different places. Across the dial the timid one arrives at the
      corner at a sixth of the speed and lands most of a tile shorter. No two
      neighbours take the jump the same way.*
- [x] **An opponent finishes the tracks people actually build.** The hard half,
      and it turned up three real faults: a **hairpin was read as a straight**
      (the geometry says a full reversal has no radius, and the code read that
      as no corner), a **step too steep to climb was driven into** rather than
      round, and a car **pinned against a cliff** sat there at full throttle for
      the rest of the race because steering is something a moving car does.
      *Verification: **88 races over the 22 tracks that ship, from every grid
      slot, none stuck**, and 48 more over twelve generated tracks. Every
      two-gate track in the game is that hairpin, which is why a bare rectangle
      was where it showed.*
- [x] **A race against opponents replays to the bit.** The golden replay has a
      second race in it now: four opponents spread across the dial, on a circuit
      none of them has seen, with no recorded inputs at all — the driving is
      worked out again from the world each time.
      *Verification: `gearstick_cli selftest --verify` races it, drives it a
      second time and gets the identical world, replays the recording of it and
      gets the same again, and compares the whole thing against a pinned hash.
      That number moves for two reasons rather than one — the physics, and the
      driver.*

## Tails

Found while implementing something else. Added when found, not when remembered.

**Every open tail has now been promoted to a phase item**, which is where the
work will actually happen — a tail is a note that something is wrong, and a
plan item is a commitment to fix it with a verification attached. They are left
here with a pointer rather than deleted, because where a problem was found is
worth as much as what was decided about it.

- [x] **What surrounds a track is undecided.** *(Closed in Phase 13: a few tiles
      of loose run-off, and then the ground falls away.)* Leaving the track now
      costs grip first and then everything.
      *Verification: `the_run_off_is_a_thing_that_stops_you` and
      `a_car_that_keeps_going_over_the_edge_is_finished`. One thing the
      perturbation turned up and is worth keeping in mind: a wall across a
      corridor no longer makes a track uncompletable, because a car can go round
      through the run-off.*
- [x] **A hard-coded demo track is in the game frontend.** *(Closed in Phase 12:
      four stock tracks ship as data in `assets/tracks/`, written by
      `tools/make_tracks.c`, and the frontend states no geometry at all.)* It is a prototype and
      it goes the moment the editor can write a real one.
- [x] **Nothing in the game frontend is a race.** ~~No start, no laps, no
      checkpoints, no finish, no HUD — two cars drive around on a demo track.~~
      *Closed by the Phase 10 front end: a lap target and a finish tick in the
      simulation, a setup screen that chooses the race, and a results table with
      everybody's time on it. There is still no HUD during the race — see the
      new tail below.*
- [x] **`assets/` is empty and exists only so the install rules have something
      to copy.** ~~Fine now; a smell if it is still true at Phase 10.~~
      *Closed, and not by filling it: the vehicles are generated into
      `src/gfx/gs_meshes.c` and the terrain is emitted as shaded geometry, so
      there is genuinely no art to load. What `assets/` holds now is
      `ATTRIBUTION.md`, written by the same script that generates the art. The
      smell was real and the answer turned out to be that the directory has
      almost nothing to do.*
- [x] **Placing a gate is not in the undo history.** *(Closed in Phase 13:
      `GS_EDIT_GATE_ADD` and `GS_EDIT_GATE_REMOVE` join the same history the
      terrain uses.)* The one edit that changes what a track *is* for scoring was
      the one you could not take back.
      *Verification: `placing_a_gate_can_be_undone_like_anything_else` and
      `a_gate_placed_inside_a_stroke_undoes_with_the_stroke` — the second
      because a gate placed mid-stroke has to come back out with the stroke and
      not on its own.*
- [x] **Import a corpus of real tracks to test against.** *(Closed in Phase 13:
      `src/core/gs_stunts.c` reads the format.)* Tracks built by people who had
      never seen this editor, which is the whole point of a corpus.
      *Verification: `a_track_from_stunts_reads_as_a_track`,
      `bytes_that_are_not_a_stunts_track_are_refused`, and
      `a_track_written_in_the_stunts_layout_reads_back_as_itself`.*
      *The original text, kept because the licence question it raises is still
      the reason no corpus is redistributed here: its repository's licence is
      unclear and must be checked first. Reference only; nothing imported ships.
- [x] **Read the original's manual for its designers' notes on the 50 stock
      tracks.** *(Closed in Phase 13.)* The lesson taken was not which tracks to
      build but that **every one of them had a reason somebody could say in a
      line** — a set that cannot do that is a set of variations.
      *Verification: twenty-two tracks ship, and `docs/GUIDE.md` prints each
      one's reason where a player will see it.*

- [x] **There is no HUD.** *(Closed in Phase 13: `src/ui/gs_hud.c`.)*
      *Verification: `the_hud_says_what_lap_it_is_and_changes_when_the_lap_does`
      and `the_hud_says_what_place_you_are_in_and_changes_when_you_are_passed` —
      both written as "it changes when the thing changes" rather than "something
      is drawn", because a HUD that shows a constant is a HUD that passes a
      screenshot test.*
- [x] **Online races assume everybody already has the same track.** *(Closed in
      Phase 11: the server hands the track out, in chunks, and a client is not
      ready to race until the rebuilt track hashes to what it was promised.)* The
      handshake exchanges player slots and addresses but not the track itself,
      so two people racing must be on the same build. A track is a few hundred
      compressed bytes and the hash is already checked everywhere else — it
      belongs in the handshake.
- [ ] **Sound has only been listened to on Linux.** *(Now Phase 13, and the same
      item as "Sound listened to on Windows and macOS" above — one thing, listed
      twice.)* The synthesiser is platform-independent and the device path is
      not. Windows and macOS need a human with speakers, and nothing in this
      repository can substitute for that. It is left for the same reason the
      phase item is.
      **The device path itself is now exercised on all three**, which is as far
      as a machine can take this: a device is opened at the end of the audio run
      and the callback has to have actually fed it. What no driver can do is
      make a noise somebody hears.
- [x] **A race that drives itself could not finish one of the shipped
      tracks.** `--session` was bounded at a flat five minutes of race time,
      written when a stock lap was under one. A stock lap is four to five
      minutes now and `jupiter run` is 5m 15s — longer than the budget — so a
      session on it stopped a lap short and reported no winner at all. The bound
      is asked of the track now, the same way the analyser asks it.
      *Verification: `jupiter run` went from "winner 255, over no" to "winner 1,
      over yes"; the tracks that already finished still do, and a race that ends
      at the flag is not slowed by a larger bound.*
- [x] **On a full grid, an opponent was launched off the map before the first
      corner.** Four cars over the shipped set lost 7 of 72, six of them wrecked
      off the north edge with no laps driven, on six of the eighteen tracks. It
      needed all four slots filled — the only grid a player races, and one no
      check had ever used. The four started abreast and level, so a car in the
      middle could be struck from both sides at once; the grid is staggered now,
      each slot a tile and a half behind the one beside it.
      *Verification: 7 stragglers became 2, and every car that was being thrown
      off the map finishes. The physics is untouched; the golden hash moved only
      because the cars start somewhere new.*
- [x] **A car a hair under the ground was frozen for the rest of the race.**
      One sat on a ramp at nought tiles a second with the throttle open for a
      hundred thousand ticks, six hundredths of a tile below the surface. Two
      rules were asking about the car where they meant to ask about the ground —
      one froze it, and correcting only that fired it into the air instead. Both
      ask about the ground now.
      *Verification: two identical cars across a level field, one on the surface
      and one just under it, went 5.07 tiles and 0.08; they now go 5.07 and
      5.07. No golden hash moved, so nothing recorded was invalidated.*
- [x] **The driver leaned on hills it could not climb.** It judged "can I get
      up this" by looking a tile and a half ahead, so a hill that starts gently
      and steepens read as climbable — and a car that stalled on the steep part
      saw the same gentle tile and a half from there. Two cars sat like that for
      the rest of a race. It asks the whole way up now.
      *Verification: the driver is asked directly rather than raced, being a
      pure function of the world: on a hill rising 0.42 of a tile at a tile and
      a half and 1.35 at three, it used to answer "accelerate" and now answers
      "brake". No golden hash moves. It was landed, withdrawn and landed again —
      that story is in `PROJECT_STATUS.md`.*
- [x] **Two opponents oscillated at the foot of a ramp for the rest of the
      race.** They alternated "back off, I cannot climb this" and "the way is
      clear, accelerate" every second or two, jittering by hundredths of a tile
      and never getting away. Backing away gains speed, the allowance for what
      counts as climbable grows with speed, and past three tenths of a tile a
      second the question stopped being asked at all — so retreating made the
      hill look easier the further they retreated. A car going backwards is now
      asked the question anyway, and has to be clear by a margin before it
      changes its mind.
      *Verification: the same car in the same place twice, differing only in
      which way it is already rolling — stopped it says accelerate, already
      backing away it says brake. Across the shipped set 7 cars of 72 failed to
      finish when this began, then 2, and now none. No golden hash moves.*
- [x] **A check that was passing by a second and a third.** The front door
      check failed once in a full run and passed on its own, which looks like a
      flake and was not: asked to say how long it took rather than only that it
      worked, it reports 28.7 seconds of the 30 it was allowed, on an idle
      machine. The budget was set when a track was 17 KB and a track is 148 KB
      now, so the handshake and transfer it covers had quietly eaten it.
      *Verification: the margin is printed on every run — "the lobby did start a
      race, correct - 28.7s of 90" — so the next time it creeps there is a
      number to read instead of an intermittent failure to argue about.*
- [x] **Every kind of ground paint is checked, not the two that were
      reported.** Two instances of one fault had been found by asking the same
      question twice, which is a sample rather than a proof. The suite now walks
      all eight kinds of paint a car can stand on and says it walked 8 of 8, so
      a ninth nobody checks turns the tree red. It found a third fault: a flame
      is exactly one tile across, so one dropped by a stationary car has its
      edge exactly on a tile line, and the sorting counted a tile the flame did
      not cover.
      *Verification: eight kinds of paint, every one 0 pixels of car painted
      over, where the arrow was 339, the flame 62 and a hazard the whole car.*
- [x] **A car sitting in a hazard was painted out of the frame.** The same
      question asked of the other thing painted flat on the road, and the same
      fault at its worst: hazards were drawn in a pass after everything else, so
      every one went on top of every car. A car in the slick it had just laid
      measured zero pixels — while the comment above that loop said hazards go
      "under everything that moves". They are painted in the sweep now.
      *Verification: the same car in the slick and on clean road, 6,491 pixels
      against 6,491, and the tests that already checked how hazards look still
      pass — only when they are drawn moved.*
- [x] **A car crossing an arrow was painted over by it.** Seen as a flicker as
      cars cross an arrow or the start line. Ground paint is sorted by the tile
      its nearest corner reaches, so an arrow two and a half tiles long could be
      painted at the tile its head reached — over anything standing on its tail
      — and a car's own tile steps up as it drives, so the two swapped places
      tile by tile. The line had been cut into per-tile pieces years back; the
      arrow never was. Every mark is now cut into pieces of half a tile.
      *Verification: a car parked on an arrow's tail showed 6,152 pixels of
      itself against 6,491 for the same car with the gates moved away — 339
      eaten by its own arrow. It is now 6,491 against 6,491, and an ordinary
      race frame is unchanged to the pixel.*
- [x] **A check that was timing the countdown and calling it the controls.**
      `gearstick_plays` asks that the car get three tiles from the line and gave
      the whole race fourteen seconds — but a race is held on the line for ten of
      them while the lights count down, so it was really judging a second and a
      half of driving. On the server's track, which starts on a slope, most of
      the movement it did measure was a held car sliding downhill. The client
      now says whether the lights are still red, and the check watches for six
      seconds of racing *after* the flag however long that takes.
      *Verification: the same run reports 734 ticks and 20 tiles of driving on
      this machine and 742 and 16 at a server, against a bar of three; and the
      same client with nothing driving its car still fails, so the check has not
      been made toothless to make it pass.*
- [x] **The store is one file with no migration path.** *(Closed in Phase 13,
      and exercised for real in Phase 14.)* There is now a schema version and an
      upgrade path: new tables appear by `CREATE TABLE IF NOT EXISTS`, new
      *columns* by `ALTER TABLE` with a default that makes an old row mean what
      it always meant, and old data is translated — the track table's `published`
      flag became the new visibility that way.
      *Verification: the schema has gone from 1 to 3 while the library committed
      under `assets/` kept opening and working, which is what
      `the_shipped_library_is_a_database_this_code_can_still_use` checks. That
      one is not hypothetical: adding an owner column with only a CREATE
      statement would have left every query naming it failing on the one
      database that ships and passing on every test that made its own.*

- [x] **The game never asked SDL for sound, so it never made any.** *(Found and
      closed while building the generator.)* `SDL_Init` was given video and
      gamepads and not audio, so every attempt to open a device failed with
      "Audio subsystem is not initialized", the mixer took its no-device path,
      and the game raced in silence on every machine there has ever been. The
      synthesiser and the music were fine and thoroughly tested; nothing was
      ever connected to a speaker. Found by asking why a captured `.wav` could
      not be made to race against the sound card.
- [x] **The audio tests passed about five times out of six.** *(Found and closed
      alongside the above.)* They opened SDL's dummy audio driver, which is
      still a driver and still runs a callback thread; that thread mixed the
      music while the test measured it, so "the same seed is the same music"
      came out true or false depending on when the callback fired. Tests now
      open no device at all.
- [x] **Two server tests raced their own server, and one MSVC warning was an
      error.** *(Found in CI while landing the generator.)* The tests bounded
      their waiting by a count of ten-millisecond delays, which is a count of
      *at least* ten milliseconds each — on a slower machine the test outlived
      the server it was talking to and failed for that rather than for the thing
      it was about. Waiting now runs on the clock, and a test server's
      self-terminate is a safety net well past anything a test needs. Separately,
      handing SDL an argument vector dropped a `const` that MSVC treats as an
      error; the cast is now written down.
- [x] **The roster was announced once, over UDP, and a client that quit never
      said so.** *(Found in CI, on macOS.)* Two faults that hid each other: the
      server sent the list of who is here only when it changed, so a single lost
      datagram left somebody racing a car that had gone home with no later
      announcement to correct it; and the goodbye message that has been in the
      protocol since it was written was never sent by anything, so quitting
      looked exactly like crashing and cost everybody else the full silence
      timeout. The roster now rides along with the ping, and a client says
      goodbye on its way out.
- [x] **The direct peer-to-peer mesh is still in the clear.** *(Found while
      sealing the client-to-server channel, closed in the same phase.)* Every
      peer link is now its own IK session, keyed from what the broker watched
      each player prove. *Verification: a four-player mesh race runs sealed and
      confirms tick for tick, a two-client race the server merely introduced
      does the same, and a well-formed datagram nobody sealed is refused while
      the real peer's traffic still arrives.*
- [x] **`PROJECT_STATUS.md`'s "What does not exist" section is years out of
      date.** *(Found while writing up the session binding.)* It still says
      there is no race, no AI, no sound, no front end and no shipped tracks, all
      of which have been built and ticked here. A reader sent to that file to
      learn what works is being told the opposite by a section that is supposed
      to be its honest half. Two commits also landed their item without touching
      the file at all, which is how it got this far behind.
      *Verification: every claim in the section is either true or gone. It now
      names the things that genuinely are not done — sound on two platforms,
      nobody having implemented a client from the transport document, the
      installer never installed on a clean machine, no tag, no rekey, no delete
      over the wire, no denial-of-service resistance — and says why the "what
      does not exist" half of a status document is the half that rots.*
- [x] **Signing in dropped an online player straight into a race.** *(Found by
      playing it, one commit after the door was built.)* Signing in sent an
      online client to the lobby, and a one-player server is ready the moment it
      is asked — so pressing SIGN IN started a race instead of showing the menu
      that had just been built. The menu is the thing you sign in *to*.
      *Verification: `gearstick_front_door` runs a real client against a real
      server and checks both halves — a client on the menu does not get pulled
      in, and one in the lobby still does, because the first check alone would
      pass if nothing ever raced.*
- [x] **The forms fought whoever was typing in them.** *(Found by filling one
      in.)* The game's hotkeys ran while a text box had the keyboard, so Tab —
      the obvious way to get from the name box to the password box — opened the
      construction set over the top of the half-filled form, and the letters in
      a driver's name toggled the ghost and the gravity overlay on the way past.
      Creating a driver also added them to the roster before checking the
      password, so a form abandoned half way through left somebody behind who
      was never finished being made.
      *Verification: the hotkeys are skipped while Dear ImGui reports it wants
      the keyboard, and nothing is added to the roster until every field is
      right.*
- [x] **The caret was invisible in every text box.** *(Found by typing into
      one.)* Dear ImGui draws the text cursor with its own colour rather than
      the text colour, and the palette here is a fixed list written before that
      colour existed — so it kept the default, which the dark inversion then
      turned black, on a black field. A box you cannot tell you are typing into.
      *Verification: a test renders an empty box with and without the keyboard
      and counts the pixels drawn in it; it reproduced the fault first and
      passes now.*
- [x] **A second online race was impossible.** *(Found by pressing Play twice.)*
      The flag saying a race had begun was only ever cleared when somebody
      cheated, so after one race the online half of the loop stopped running
      entirely: the lobby froze on what it last heard and nothing ever started
      again. Nothing noticed while a client went straight into one race and
      stayed there.
      *Verification: asking for the lobby again clears it, unless the last race
      is still being agreed with everybody else.*
- [x] **Screenshots and demo sessions wrote into the player's roster.** *(Found
      by somebody having drivers they never made.)* `--screen` and `--session`
      invent drivers so there is somebody in the picture and somebody in the
      results, and every such run saved them into the real store. A machine
      being told what to draw now reads the store and leaves it alone.
- [x] **The profile screen let you edit everybody.** It listed the whole roster
      with an edit and a remove on every row, so signing in as anybody let you
      rename, repaint and delete the other people on the machine. It shows the
      driver who is signed in, and only them.
- [x] **Records named a track by its hash.** A track is known by its hash and
      that is what a record is filed under, but "4ac9ccc660fae00f" is not
      something anybody can talk about. The library's name for it is used where
      there is one.
- [x] **The login screen listed everybody on the machine.** A list of drivers
      answers "who is here" to whoever sits down, which is half of what a
      password protects. The name is typed now, alongside the password.
      *Verification: a name that is not on the roster and a name with the wrong
      password are refused with the identical message, so the box cannot be
      asked who is on this machine one guess at a time.*
- [x] **A server whose output nobody was reading stopped serving.** *(Found by
      Windows CI, where the front door check had never passed.)* The server
      draws a dashboard four times a second; sent into a pipe nobody is
      draining, that fills the buffer and the next line blocks the server inside
      a write, where it answers no knock and sends no track. A Windows pipe
      holds about one second of dashboard and a Linux one about sixteen, which
      is the whole reason one platform failed and three passed. The dashboard is
      now for a terminal; anything else gets a log — the lines that mark events,
      and one a minute saying the server is still here. Its output is also
      unbuffered now, because the key line a client cannot connect without had
      only ever escaped its buffer on the back of the flood.
      *Verification: leaving the pipe unread for twenty-five seconds before
      joining reproduces the Windows failure on Linux, and does not once fixed;
      and a new check runs the server with its output going into a pipe nobody
      reads, requires it to stop when it was told to, and requires what it wrote
      to be less than a pipe holds — which the old server fails on every
      platform, including the ones where the fault never showed.*
- [x] **And the same check was red for a second reason.** *(Found by the server
      log the fix above taught it to print.)* Both halves of the check shared
      one server with one seat in it. Killing a process on Windows gives it no
      chance to say goodbye, so the first client's seat was still held when the
      second knocked, the second was told the server was full — and a refused
      client does not knock again, by design. The check was failing for
      something that had nothing to do with the rule it exists to pin, and
      passing elsewhere for a reason it did not have on Windows. Each half now
      gets a server of its own, and both clients are killed outright on every
      platform rather than being asked politely on some of them.
      *Verification: killing the first client instead of asking it to leave
      reproduces the Windows failure on Linux exactly, down to the server's
      "went quiet"; with a server each it passes with the harsher teardown
      everywhere, and pointing it at a server that needs two players still fails
      the second half, with both logs printed.*
- [x] **The library screen was taller than the window it was in.** *(Found by
      photographing every screen and looking at the pictures.)* Its panel was
      sized to hold the whole library, so twenty-two tracks made it taller than
      the window and centring it put the first track above the top edge — on a
      panel that cannot be moved or resized, which means the first track in the
      library could not be chosen. The title and drivers screens were the same
      fault from the other end: a height set by hand, with a button added since
      that fell off the bottom. Panels are now never bigger than the window, and
      the library list scrolls inside its own box instead of growing the panel.
      *Verification: every screen is drawn and measured — its panel has to lie
      inside the window and have nothing below the fold at the size the game
      opens at, and to still lie inside the window when it is dragged to half
      that. It failed on five of the eight screens before the fix.*
- [x] **The line that says you set a record was cut in half.** *(Found by racing
      a whole session and looking at the results.)* It read "lap + race r": the
      note sat in the last column of the results table, which was given whatever
      was left over, and what was left over was not enough for the sentence. The
      column is now as wide as the widest thing that goes in it, asked of the
      font, and the panel is worked out from what its table needs rather than
      set by hand.
      *Verification: a session raced end to end and photographed, with the whole
      sentence on it; and the panel test now measures the results screen with
      the note showing, which is the widest that screen gets.*
- [x] **The construction set's gravity dial was labelled "gravity (x Eart".**
      *(Found by opening the editor and looking at it.)* The one dial whose
      units matter had its units cut off at the edge of the palette. Every
      slider now stops where the longest label starts, that width being asked of
      the font rather than counted by hand, so they fit however wide the palette
      has been dragged — and they line up in a column, which was not the point
      and is better anyway.
      *Verification: the palette photographed with the whole label on it.*
- [x] **Nothing anybody did was ever saved.** *(Found by playing it: the log
      said it could not write the store, and gave no reason because there was
      none to give.)* The game held room for the roster, the records and four
      kilobytes of everything else — and everything else is the track library,
      which is four kilobytes for one track and ninety for the twenty-two that
      ship. So every save was refused, on every machine, from the day the
      library joined the store, and the message blamed the disk. It asks how big
      the store is now, and the two ways of failing say different things.
      *Verification: a store with a library of shipped tracks in it is saved and
      read back whole, every track by name and identity — and a buffer that does
      not fit is refused rather than half-written, because a store with three of
      somebody's four tracks in it is worse than one that failed loudly.*
- [x] **A race with nobody else in it froze after two seconds.** *(Found by
      racing on a server of your own.)* The rollback only ever agreed a tick
      when somebody else's packet arrived, and in a one-player race nobody
      writes in — so nothing was ever agreed, and after one window of ticks the
      race stopped dead with the controls doing nothing. Two and an eighth
      seconds, every time. A machine now agrees what it already knows on its
      own.
      *Verification: three windows of solo racing with no stall, producing the
      same world a machine with no network at all would have; and a race with
      company still waits for the other machine, which the test that pins that
      has always checked.*
- [x] **An online race was built from your own setup screen.** *(Found in the
      same race: two cars on a one-player server.)* When the front door was
      built, online players started going through the front end — and the race
      went on being built the way a menu-less client built it, which after that
      change meant reading the local setup screen. On a one-seat server that
      invented a second car, and the ghost of it on the start line is what
      dragged the camera off your own; in a real two-player race the two
      machines would build different worlds, which is the one thing rollback
      cannot recover from. An online race is the server's race again.
      *Verification: the client says the grid it built as the race starts, and
      the front door check refuses a race whose grid is not the size the server
      said — which the old code fails.*
- [x] **The condition bar ran into the edge of the HUD.** It was drawn to a
      padding somebody wrote down, and the style pads a window by nearly three
      times that, so the bar ended hard against the frame with no margin.
      *Verification: a rendered frame with none of the bar's colour in the last
      few pixels before the frame and plenty of it inside, so the check cannot
      pass by there being no bar at all.*
- [x] **A race with no car in it.** *(Found by playing it: track, HUD, running
      clock, no car.)* The race camera held its height at zero while the camera
      that sets you on the grid follows it, so on a track whose start line is
      eight tiles up the car was drawn eight tiles up — off the top of the
      window, with the camera otherwise exactly where the car was. On flat
      ground at height zero, which is every picture anyone had taken, it looked
      perfect. The camera follows the ground fully and the air only partly now,
      which is what it was always meant to mean.
      *Verification: every driver can see their own car on ground that is not at
      height zero, for one to four cars, split screen and merged — and a car in
      the air is higher up its own screen than one on the ground and still on
      it, so the fix cannot be "follow everything" and lose the jump.*
- [x] **A wrecked car had no way out.** A wreck ends nothing, so nothing moved
      you on, and the one key that means "out" went to a setup screen that
      decides a race a server owns. Where back goes is a rule with a test now,
      and the HUD says `Esc leaves` when you are wrecked.
      *Verification: out of a race is the setup screen on this machine and the
      lobby when the race is somebody else's; everything else backs out to the
      title; the title and the door are where leaving belongs.*
- [x] **The game can play itself, and say what it is showing.** Every fault
      found by playing this was in the thirty seconds after the green flag, and
      nothing was looking there. The AI can take this machine's wheel
      (`--autodrive`), the client reports what is on screen once a second
      (`--trace`), and a track can be opened by name (`--track`).
      *Verification: `tools/play_check.py` races on this machine and against a
      real server and asserts what a person checks in the first five seconds — a
      race starts, the clock keeps advancing, the car is on screen every look,
      it gets somewhere, nothing stalls. Against the old camera it fails with
      the diagnosis in one line.*
- [x] **A car wrecked in the air never came back on screen.** *(Found by the
      trace, in minutes.)* A car that goes over the drop stops where it is, so
      it can hang eight tiles up for the rest of the race — and a camera that
      follows a third of the air leaves it above the top edge and keeps it
      there. The follow is capped by the pane it has to stay inside now. Two
      cars far apart in *height* also counted as being together, so the screen
      stayed merged and framed the air between them; height is part of how far
      apart two cars are now.
      *Verification: a car down a drop does not take the camera off the other
      one, and a car in the air is higher up its own screen than one on the
      ground and still on it — both fail without the fix.*
- [x] **`--players` had been doing nothing.** The race was built at start-up
      before the game had decided whether this run has a front end, so it always
      took the menu's two cars. Four-player split screen was unreachable from
      the command line while the help offered it.
      *Verification: the screenshot pass — one, two, three and four cars, and
      the camera invariant checked over all twenty-two shipped tracks by racing
      each of them with the trace on.*
- [x] **A dead car went on counting its lap time, and said nothing else.**
      *(Found by dying.)* A wreck ends nothing in the simulation, so the clock
      kept running and the screen looked like a very slow lap rather than a
      dead driver. It says **YOU DIED** now, the lap clock stops, and the two
      keys that do something about it are named: R restarts a race on this
      machine, Escape goes back to the menu — or to the lobby, when the race
      belongs to a server and restarting is not one machine's to do.
      *Verification: the HUD is drawn in every state it has — racing, wrecked,
      waiting, finished, and the combinations — and asked how much of it ended
      up below the bottom of its own panel. It was eleven pixels short in every
      state before this, and a whole line short with the wreck message on.*
- [x] **A race whose other machine has gone says so, and ends.** Waiting used to
      be silent and endless: the rollback stops the world when nobody else is
      talking, and a stopped world with nothing said about it looks exactly like
      a game that has crashed. It says how long it has been quiet, offers the
      way out, and after twenty seconds — longer than the server's own patience
      — it stops waiting and goes to the results.
      *Verification: the give-up is longer than the fifteen seconds after which
      the server drops a silent client, so a client having a bad moment is
      dropped by the server first and this only fires for somebody genuinely
      gone.*
- [x] **A track now says where it ends and which way it goes.** *(Asked for
      after dying repeatedly at the same spot.)* The edge of the road was a
      change of shade, which a driver at speed does not see coming — it is a red
      and white kerb now, one block a tile, all the way round the authored
      ground. And the route was drawn nowhere at all during a race: the gates
      existed in the simulation and as a white line in the construction set, so
      arriving at a track told you nothing about which way round it went. Every
      gate is drawn on the ground with an arrow through it pointing the way, and
      the start and finish is chequered so it is not just another gate.
      *Verification: a track is rendered and the three things are counted by
      their colours — the kerb's red, the arrow's yellow and the chequer's
      white. None of them was on the screen at all before this.*
- [x] **A car is drawn whole, and drawn where it is.** *(Reported three times
      over, as "jerky", as "an artefact around the car", and finally as "the
      background comes over the bonnet every few seconds".)* Three unrelated
      faults. The camera followed the settled simulation state while the cars
      were drawn interpolated between two of them, so it was always aimed a
      changing fraction of a tick away from what was on the screen. The vehicle
      meshes were built from boxes that sink into one another, and a painter's
      sort cannot order interpenetrating solids, so the windows and sills
      surfaced through the bodywork and crawled about as the car turned. And a
      car was sorted into the terrain by its centre tile despite being longer
      than one, so the ground it was standing on was painted over its bonnet.
      *Verification: the camera holds a car within half a pixel as the frame
      lands anywhere between two ticks; only the outside surface of each
      vehicle is generated now, so there is no buried face left to sort wrongly;
      and the same car photographed at a tenth and at nine tenths into its tile
      covers the same pixels either way - before this it lost its bonnet,
      headlight, bumper and both front wheels.*
- [x] **A start line and a finish line that are different things.** *(Asked for
      directly: "it looks like the finish line is rendered at the start line",
      and "it is confusing whether beginning and end is".)* The grid sits three
      tiles behind the line a lap is measured on, and that chequered line was
      the only one drawn — so the first thing anybody saw on arriving at a track
      was a chequered flag, which everywhere means *finished*. There is a plain
      white line across the grid now and the chequer stays ahead of it, with a
      flag at each end. The other gates stopped pretending to be finishes too:
      the solid blue band across the road is a post at each edge instead, open
      in the middle, with the direction arrow kept.
      *Verification: each line is sampled where it actually lies — the grid's
      line must be white with no chequer in it, and the finish must have both
      colours. And a car five tiles short of a gate keeps all of itself when the
      line is painted over the ground it is standing on; before this the far
      half of the line was drawn after the car and swallowed it.*
- [x] **A race begins when everybody is ready.** *(Asked for directly.)* A race
      used to simply be, from tick zero, so arriving at a track meant already
      being late. A light tree stands beside the grid now: three lamps light one
      a second, all three go green together, and until they do no input reaches
      any car at all. The lap clock waits for the flag too — it used to count up
      while the cars sat still on the grid.
      *Verification: full throttle and full steering held down through the whole
      countdown moves the car not one fixed-point unit, and the same input moves
      it the moment the lights change. The lamps are counted in the rendered
      frame at each second. And a race nobody counted down is held for no time
      at all, which is why every replay ever recorded still lands where it did.*
- [x] **Tracks that go somewhere.** *(Asked for after driving them: "the track
      doesn't seem to go anywhere", and "it is useless if the tracks are not
      real examples".)* Every generated track used to be an open field with two
      gates on it, one near each edge — and a "lap" was counted on reaching the
      far one, so lap two meant driving all the way back with nothing marking
      the way. A track now says whether it is a **loop** or a **path**: a loop
      has one chequered line that is both its start and its finish, a path has a
      plain start line at one end and a chequered finish at the other. The route
      is cut into the ground with its own road surface, so it can be seen, and
      everything that is only a waypoint is marked at its edges rather than
      barred across.
      *Verification: all 200 seeds build a driveable track, now with six to ten
      gates over a much bigger field rather than two. The route was checked for
      completability before and that was the whole mistake — completable is not
      raceable, and an open field with two gates is trivially completable.*
- [x] **A parts box, after the original's.** *(Asked for directly, with a
      picture of the original's PARTS BOX.)* The editor had brushes, which shape
      ground but do not build a road — laying a straight by hand means keeping
      forty corners level yourself. There are nine pieces now: straight, corner,
      ramp, crest, dip, and the four route pieces — start line, finish line,
      **combined start / finish** and checkpoint. Choose one, modify its turn,
      width, length, rise and surface, and drop it. Dropping a combined line
      makes the track a loop; dropping a separate start or finish makes it a
      path. A start line becomes the first gate wherever it is dropped.
      *Verification: a whole piece undoes in one step and redoes to the same
      track; a road piece is level across its width; a start line dropped after
      everything else is still where the race begins; and a piece that will not
      fit changes nothing. Intersections with overpasses are **not** included —
      see PROJECT_STATUS.md, they need a second height per tile, which the
      terrain does not have.*
- [x] **A finish line that fires, and a lap that means a lap.** *(Reported: "I
      drove across the finish line and the game did not recognise it.")* Gates
      were being laid narrower than the road they cross, so a car keeping to the
      outside of its own road drove *past* a checkpoint — and because gates count
      in order, the finish then did nothing when it was reached. Two more faults
      came out with it: a lap of a loop counted the crossing a car makes leaving
      the grid, so a three-lap race ended after two; and a path was being raced
      for three laps when arriving is the whole race.
      *Verification: every gate on every generated track is wider than the road
      it crosses. The analyser had said all these tracks were fine, because it
      races an AI that aims at gate centres and so never missed one.*
- [x] **A racing tree, ten seconds, red then amber then green.** *(Asked for.)*
      Three seconds was not long enough to read the first corner, and counting
      lamps said how long was left but not what to do with it.
- [x] **Escape backs out of a race instead of restarting it.** *(Reported.)*
      Leaving an online race went to the lobby, and a full lobby started a race
      immediately — so leaving put you back in the race you had just left. The
      lobby waits now and offers Race to go again.
- [x] **A crossroads in the parts box.** Flat, both ways open. An overpass is
      still not possible: the terrain holds one height per corner and two roads
      at different heights need two.
- [x] **An online race that can be finished at all.** *(Reported twice: "I drove
      through the finish line and it still didn't recognise it.")* An online
      race is built from the server rather than from the setup screen, and that
      path set the grid and nothing else — so the lap count stayed at zero, and
      zero means a race with no finish line. Every online race was unfinishable.
      A path is now raced once, end to end, and the setup screen stops offering
      a lap slider for one.
      *Verification: the analyser used to call a track completable if the car
      had crossed the start line once — which on a loop happens seconds after
      leaving the grid, and is why two rounds of unraceable tracks passed. It
      now requires a whole lap or an arrival, and a new test races the AI and
      requires it to actually finish.*
- [x] **The construction set, reached from the screen about tracks.** *(Asked
      for: "hitting Tab off the main menu to edit a track is stupid and not
      obvious".)* New, Edit and Delete sit beside Load now. New starts a blank
      field; Edit opens the chosen track — or, for one that came with the game,
      a copy of it, because the library a player came with should still be there
      after an afternoon of building. A shipped track cannot be renamed or
      deleted either.
      *Verification: the flag that says where an entry came from survives being
      written and read back, or the protection would last until the game was
      next started and then quietly stop.*
- [x] **Handing a track to somebody.** The code is always there to copy. With a
      server there is Publish to everybody, Take it down, and a button per
      person in the lobby to hand it to them or take it back — named by the key
      the server watched them prove, not by a string somebody typed.
      *Verification: the screen-fitting test caught the panel overflowing once
      the new controls were on it — after its fixture was made to actually pick
      a track, which it had never done.*
- [x] **A Race button that did nothing.** *(Reported, with the fair complaint
      that it should have been tested.)* It compared a player count against a
      capacity, and before the server answers both are zero — so it offered
      Race to somebody still knocking and did nothing when they pressed it.
      *Verification: the condition is a predicate now rather than an expression
      buried in the drawing, because a predicate is a thing a test can call —
      which was the real failure. Every state the lobby has is checked, and the
      version that shipped fails two of them.*
- [x] **Getting around the track, and a box of parts you can see.** *(Reported:
      "I don't have the toolbox to select from, and I don't have the ability to
      scroll around the track".)* Right or middle drag pans, the wheel zooms,
      and the parts box is its own window in the top right with all nine pieces
      as buttons — after the original's PARTS BOX beside the COURSE.
- [x] **Tab is no longer the front door to the editor.** *(Asked for.)* New and
      Edit on the tracks screen are. Tab still runs the build-drive-build loop
      while you are working on something, and does nothing once you are not.
- [x] **A window with its own icon.** *(Asked for.)* A gear lever — a red knob
      on a shaft out of a gaiter, over the shift pattern's gate — drawn by
      `tools/make_icon.py` rather than by hand, so it is still true that no art
      in this game came from anywhere else. It needed an image library:
      SDL_image, cut down to PNG and nothing else.
      *Verification: the icon ships, decodes, is square, is not one flat colour,
      and has a transparent surround rather than being a full square — which is
      what lets it sit on a title bar of any colour.*
- [x] **A results screen you can leave.** *(Reported: finishing a race, choosing
      another track and pressing race put you straight back on the results.)*
      Going to the lobby cleared the "results worked out" flag while the
      finished world was still loaded, so the next frame ran the end of the race
      again — submitting the same result twice and forcing the screen back.
      *Verification: a race that is over stays over however long it is stepped,
      and a car that has finished is timed once and never again.*
- [x] **An online flow with no dead ends.** Racing is not this machine's to
      start while it is on a server, so the tracks screen loads rather than
      races, the results screen offers **Back to the lobby**, and the local
      setup screen is offered offline only.
- [x] **A door nobody answers says so.** A wrong key means the server cannot
      decrypt what we sent and has nothing to reply to, so the lobby knocked
      silently forever. It now names the three things it could be.
- [x] **The tracks screen puts a whole library between you and its buttons.**
      *(Found walking the front end by machine — see Phase 17. Closed by giving
      the pad a way out.)* Escape is a key and a pad has none, so somebody on a
      pad could only leave by walking down through every track they own. **The
      pad's cancel button is back now** — except during a race, where that same
      button is the brake.
      *Verification: the cancel button counts as back and no other button on the
      pad does, in a race or out of one; and the brake really is that button, so
      moving it and leaving this rule behind fails rather than surprising
      somebody in the first corner. **Only a key can ask to quit** — the title
      screen prints "Escape  quit" and means it, and a pad's cancel doing the
      same would close the game from the title on the button everybody presses
      reflexively.*
- [x] **Login and tracks are exempt from the no-trap check.** *(Closed in
      Phase 17.)* Tracks stopped being exempt when the walk learned to press
      Escape; the door stopped being exempt when the walk that carries the
      password had what it learned folded into the same graph.
      *Verification: 0 traps and 0 stranded over **every** screen, with no
      exemption anywhere. One exemption is left in the whole set of properties
      and it is correct: no button leads to the results, because finishing a
      race is what takes you there.*
- [x] **Half the planets vanish in a small window.** *(Found by photographing
      every screen at 640×480 — the size the panel test itself calls "what
      somebody dragging a corner gets".)* These panels promise that whatever
      does not fit scrolls, and only kept the promise downwards: the setup
      screen wants 800 pixels across, got 624, and threw the difference away.
      Mars, Venus, Neptune and Jupiter were not on the screen, and neither were
      the last two paint colours on every driver's row. The panels scroll
      sideways now, and the grid keeps the width it was designed at rather than
      squeezing a column until it clips its own contents.
      *Verification: at 640×480, every one of the 121 controls across all eight
      screens is wholly on screen at some scroll position the window can be put
      at — the panel is walked over a grid of scroll positions half a window
      apart in both directions, and the test says how many controls it covered
      and how many sit inside lists that scroll themselves. It named four before
      the fix and none after.*
- [x] **The build was broken on Windows and macOS for four commits.** *(Found by
      looking at CI rather than at `ctest`.)* Locally green the whole time: one
      cast in the driver that MSVC refuses and gcc and clang accept without a
      word, so nothing on Windows compiled at all; and a test sandbox built out
      of `HOME` and friends, which is how Linux decides where a player's files
      live and is not how macOS decides, so the suite wrote to a real
      preferences directory there. The game reads one setting for that now,
      which means the same thing on all three platforms, and the test binaries
      set it themselves rather than relying on being launched by `ctest`.
      *Verification: all three platforms build and pass; running a test binary
      straight from the build directory, with nothing set in the environment,
      keeps its files inside the build tree and says where they went.*
- [x] **Four players, and every HUD drawn over the player below.** *(Found by
      measuring the HUD in the view it belongs to rather than in a window of its
      own.)* Every state of it had been checked for fitting its box, and always
      in one view filling the whole screen — but four players get a quarter of
      the screen each, and at the size the game opens at six of the twelve
      states were taller than the quarter they were drawn in, showing one
      player another player's lap time. The panel is drawn at whatever fraction
      of itself fits now: text, gaps, padding, bar and width all together, so a
      small view gets the same HUD smaller rather than a squashed one.
      *Verification: four cars in the four corners of a track, the screen split
      four ways, and all twelve states of the HUD drawn in all four views at
      both the size the game opens at and a smaller one — 96 panels, every one
      of them inside the view it belongs to.*
- [x] **A scrollbar along the bottom of screens that fit.** *(Found by
      photographing the screens again after the last fix — it was that fix's own
      doing.)* Letting every panel scroll sideways also gave every panel with a
      rule drawn across it a permanent scrollbar with nothing to scroll, because
      a full-width rule is exactly as wide as the window holding it. Panels ask
      for the sideways scrollbar only when they were actually made narrower than
      they wanted to be.
      *Verification: a window that can be scrolled shows the bar that says so,
      and one that cannot does not — checked on every screen from every starting
      state. The reachability test could not have caught either fault before:
      it moved the panel itself, which a person cannot do without the bar.*
- [x] **Every screen measured with the longest name anybody can type.** A driver
      name holds fifteen characters and a track name forty-seven, and every
      layout had only ever been measured with "gavin" and "track number 7" in
      it — the narrowest a screen can be, and a name is the one piece of a
      screen the person using it chooses the width of.
      *Verification: an eighteenth starting state fills every name in the menu,
      the library, the records and the lobby with the widest glyph there is.
      Nothing overflowed and nothing became unreachable, at either window size.*
- [x] **The construction set was laid out for one screen.** *(Found by measuring
      its panels at sizes other than the one they were designed at.)* At 960×600
      — an ordinary window — the parts box sat 304 pixels off the right-hand
      edge, and at 640×480 nineteen pixels of it were on screen and the rest
      was not. Nothing scrolled, because a window knows what did not fit inside
      itself and has no idea it is hanging over the edge of the display. The
      panels are never opened bigger than the screen now, never draggable or
      resizable past its edges, and are put back inside if the screen shrinks
      under them — while staying the player's to move, which is what a tool
      panel should be.
      *Verification: all three panels measured under every brush at four window
      sizes down to 400×300 — 72 measurements, each wholly on the screen and
      each showing a scrollbar exactly when it has something to scroll.*
- [x] **Every window measured, not just every panel.** A list or a bordered box
      inside a panel is a window in its own right, with its own edges and its
      own scrolling, and the three in this game had never been asked what they
      were hiding sideways. Now every window a screen draws is found from the
      frame itself and held to the same rule as the panels.
      *Verification: 183 windows across eight screens and eighteen starting
      states, each showing a scrollbar exactly when it has something to scroll —
      and the test also states how many of them can actually scroll (27
      sideways, 93 down) and fails if that is none, because a rule about
      scrollbars proves nothing where nothing scrolls.*
- [x] **Six of the nine grounds sounded like pavement.** *(Found by asking what
      the sound tests actually covered.)* The synthesiser knew pavement, dirt
      and ice — the three that existed when it was written — and everything
      else fell through to pavement. Sand, gravel, rock, dust, slush and grass
      each have their own grip and their own way of wearing, and by ear all six
      were the same ground. They have their own voices now, and a tenth surface
      fails to build rather than silently sounding like the first.
      *Verification: every surface is walked, not three of them — each has to be
      audible, and all thirty-six pairs have to be tellable apart by loudness or
      by brightness. Dirt is still the loudest and ice the quietest, ice is
      still a hiss where dirt is a rumble, and rock is the deepest thing there.*
- [x] **Four cars at full noise, on every ground and in every machine.** The
      check that nothing the synthesiser produces can blow a speaker used dirt
      in a sprint car, dirt having been the loudest surface at the time — and
      three of the six added since are louder than it.
      *Verification: 54 mixes, nine grounds by six machines, four cars each all
      sliding and all being struck; and nine more with a different tune playing
      underneath. Nothing clipped and nothing produced a NaN.*
- [x] **Nothing had ever read a gamepad.** *(Found by building the tree under a
      coverage tool and asking which files the suite never touches.)* Opening a
      pad, closing one, hot-plugging, and every line that reads a physical
      control had never run in any test, on any platform — a quarter of the
      input file. Four players on one sofa is the shape of this game, and all
      of it rested on code nothing had executed. SDL can make a gamepad that
      exists only in software, so now four of them get plugged in.
      *Verification: four pads plugged in one at a time and a fifth refused;
      each pad drives its own car and no other; every button bound by default
      does what it is bound to; both triggers stand in for the buttons they
      stand in for; the stick steers past the deadzone and not before it; and
      somebody trips over a cable mid-race, after which the pad that was third
      drives the second car and nothing reads a closed one.*
- [x] **Escape went somewhere different from the button beside it.** *(Found by
      following what the client does with Escape down to the rule that decides
      it.)* The records screen remembers which screen opened it so its Back
      button can return there, and Escape threw that away and went to the main
      menu — as did the pad's cancel button, which is the same rule. And the
      results of a server's race offered "Back to the lobby" on screen while
      Escape went to the main menu, out of the room the next race is decided in.
      Both are one rule now, written once and used by the buttons and by
      Escape alike.
      *Verification: every screen walked on a server and off one, with the
      construction set open and shut, and the records screen walked from all
      nine screens as the place it came from. The test also fails if a screen is
      missing from its own table, so a tenth screen must have its way out
      chosen rather than inheriting one.*
- [x] **Rebinding a control from the keyboard could only ever bind Space.**
      *(Found by asking the coverage build which player-facing code had never
      run — the capture was at a third of its lines.)* A capture starts the
      instant you press the control that says "press something...", and you
      pressed that control *with something*: Space or Enter from a keyboard,
      the bottom button from a pad. It was still held on the next frame, so the
      control bound to it before you had touched the key you meant. A player
      with a mouse never saw it; everyone else could not rebind anything. A
      capture now waits for everything to be let go before it accepts anything.
      *Verification: all 510 scancodes and all 26 pad buttons can be bound to —
      not a handful, because a key that cannot be captured is a control
      somebody cannot have. Escape still means leave it alone, and not when
      Escape is merely the key that started the capture.*
- [x] **A circuit and a sprint over the same ground were one track.** *(Found by
      walking every kind of edit through undo — changing a track from a loop to
      a path registered as "changed nothing".)* A track is known by what it is,
      and what it was did not include whether its gates make a lap or a run.
      The library treats two tracks with the same identity as one, so building a
      lap, saving it, turning it into a run and saving that under a second name
      left you with one track carrying the second name on the first track. Best
      times were pooled the same way — a lap of a loop beside a run from end to
      end.
      *Verification: the two are now two entries in a library, each the track it
      says it is, and the test states the fault it was written for. Nothing
      about the physics moved. Codes shared with the released beta still open,
      because the reader accepts the answer the hash used to give.*
- [x] **Every kind of edit can be taken back and put back again.** Two of the
      seven kinds — moving a gate along the route, and changing whether the
      track is a lap or a run — had never been called by any test at all.
      *Verification: all seven walked, each applied, undone and redone with the
      track compared to the bit each time, and twice round to prove the history
      is left usable. The test counts against the number of kinds, so an eighth
      fails the tree rather than being forgotten.*
- [x] **The shipped stock tracks were two short.** Which generated tracks ship
      is decided by racing them — every vehicle has to be able to finish — so
      the set depends on the simulation, and the simulation has changed several
      times since those files were written. The job that keeps committed
      generated files honest was not watching the simulation.
      *Verification: the chooser is re-run and its output committed; the check
      now runs whenever anything in the simulation changes.*
- [x] **A track is identified by everything that is on it.** *(The guard the
      route byte got past.)* Naming the one case that went wrong catches that
      one case; the general fault was that something was added to a track and
      whatever decides what a track *is* was never told. Every byte of a track
      is now flipped, and the identity has to move exactly when it should.
      *Verification: 315 bytes are what a track is and 16842 are room the arrays
      have and nobody filled, with every one of them behaving as claimed — so
      anything added later is either part of a track's identity or is written
      down as deliberately not, and cannot be neither.*
- [x] **A race is identified by everything that decides it.** The same sweep as
      the one for tracks, on the hash two machines compare to find out they have
      stopped agreeing — because a field missing from that is a disagreement
      neither machine can see.
      *Verification: 8325 of a world's 8848 bytes are shown to decide the race,
      the rest being padding, cars nobody added, and one named field. A car is
      56 bytes and 52 are read, checked as arithmetic rather than by offsets, so
      adding a field to a car fails the tree on any compiler. The one field left
      out — when the lights go green — is named with its reason, and the thing
      that covers it is asserted rather than assumed: take that away and the
      test goes red.*
- [x] **Four things to leave behind.** *(Found by asking which enums no test
      walks — nothing had ever asked about hazard kinds.)* The mine was written,
      hashed and tested, and no player could drop one: the fire button was
      hard-coded to oil. There are four now — oil, a mine, smoke that hides the
      ground, and fire that burns while you are in it — and one button chooses
      between them, a tap dropping what is selected and a half-second hold
      moving the selection on. Each car carries a count of each, spent as they
      are used.
      *Verification so far: every kind can be carried, selected without anybody
      pressing anything, and dropped by a tap; a hold changes the selection and
      drops nothing; running out moves the selection on and running out of
      everything selects nothing; fire hurts more the longer you stay in it and
      then burns out, where a mine goes off once; smoke changes nothing about
      how a car drives. A race with the weapons off is every car carrying zero,
      which is why every race that came before behaves identically.*
      The race setup screen carries the switch and the four counts, and a race
      built from it arms everybody on the grid.
      *Verification: weapons off is every car carrying nothing however the
      counts are set; on, every car including the one added last carries what
      the screen says; one count at zero is that weapon absent and the others
      still there; and a race with weapons files its times apart from a clean
      one — while a race without them files exactly where it always did, so no
      record anybody has already set is lost.*
      A recording carries the loadout, so a race with weapons in it replays as
      the race it was.
      *Verification: a recorded weapons race, driven with the button being
      tapped, re-races to the same world hash — from the recording in memory and
      from the bytes it is written to. A recording made by the released beta,
      which came before weapons, still reads and reads as a race with none.*
      **Finished by the four items under Phase 19 below:** they can be seen,
      they can be heard, the screen says what you are carrying, and the
      computer uses what it has.

## Phase 19 — Weapons a player can see, hear and be beaten by

The simulation, the dial and the recording are done. What is missing is
everything a person actually experiences: two of the four weapons are invisible,
none of them makes a sound, the screen never says what you are carrying, and the
computer never uses any of it.

- [x] **Smoke and fire you can see.** The renderer knows two kinds of hazard —
      oil, and a small dot for everything else — so smoke and fire currently
      look like mines. Smoke is the one that has to read at a glance, because
      hiding the ground is the whole of what it does.
      *Verification: each of the four is drawn, told apart from bare ground and
      from each of the other three, and smoke comes out pale and near-solid
      because hiding the ground is the whole of what it does. Each is also drawn
      at exactly the size the simulation will catch you at — measured by drawing
      the same ground with and without it, so what a player sees is what hits
      them. Every kind walked, so a fifth has to be given a look rather than
      inheriting whichever case came last.*
- [x] **Weapons you can hear.** `src/audio/` has never heard of a hazard: there
      is no sound for dropping one, for a mine going off, or for fire burning.
      A mine you cannot hear behind you is a mine that feels like the game
      cheating.
      *Verification: dropping each of the four is audible over the race going on
      around it, and all six pairs of them are told apart by loudness or by
      brightness. A mine going off is more than half again as loud as laying
      one. Fire is heard while it burns and fades once it is out, rather than
      stopping dead. And four cars dropping and detonating on top of four
      engines still fits in a speaker, on every ground and in every machine.*
- [x] **The HUD says what you are carrying.** The control is explained once, on
      the setup screen, and never again — and there is nothing on screen saying
      which weapon a tap would drop or how many are left. A hold that silently
      changes something invisible is not a control.
      *Verification: the row appears only when there is something to carry and
      goes when the last one is spent, rather than sitting there saying zero. It
      names every kind by the name the setup screen used — one list, so the
      screen you choose on and the screen you race on cannot drift apart — and
      counts down as they are used. And the panel is measured in all twenty-four
      states now rather than twelve: every state with weapons and without,
      across two, three and four players at two window sizes, 432 panels, each
      inside its own view with no hole in it.*
- [x] **Opponents that use what they are carrying.** `gs_ai_drive` never presses
      the button, so a derby against the computer is one armed human and three
      unarmed cars. The driver needs a rule for when leaving something behind is
      worth it — roughly, when somebody is close behind.
      *Verification: an opponent with somebody right behind it leaves things on
      the road; the same opponent with the road behind it empty leaves fewer;
      one carrying nothing leaves none at all. Four armed drivers racing each
      other still get round rather than paving the track and stopping.*
      *And the opponents hash did **not** move, which is better than expected:
      carrying nothing is pressing nothing, so a race with the weapons off is
      exactly the race it was before.*

## Phase 20 — The four-player camera, when it cannot merge

The one open question in `FEATURES.md` that is a live defect rather than a
choice: *"what the merged four-player camera does when it cannot merge. The
failure mode is the design, and it has not been thought about yet."* Four cars
that will not fit one view split into four, and nobody has decided whether that
is right, what happens on the way, or what a two-player race does when one car
is left behind at the far end of a long track.

- [x] **Decide what happens, then make it happen.** What it did was give three
      players the four-player grid with one quarter left blank — a quarter of
      the window, for the whole race, while the three people racing were each
      squeezed into a box a quarter the size. Nobody chose that; it fell out of
      a loop that stops early. Three players get three columns now.
      The decision, in three rules: every pane is the same size, because an
      unequal pane is an advantage and this is a game played on one sofa; the
      panes fill the screen apart from the divider; and no pane overlaps
      another, or two players are looking at the same pixels and one of them is
      wrong.
      *Verification: every player count from one to four at three window sizes —
      twelve layouts, each filling the screen, sharing it evenly, and never
      overlapping. The old rule that three and four share a grid so a joining
      player does not rearrange the screen is gone, with the reasoning recorded:
      nobody joins a race, so that rearrangement happens between races where it
      costs nothing, and it was being paid for with a blank quarter all race.*

## What is left, and who has to do it

**Everything the game needs in order to be itself is done.** What follows is
two different kinds of open, and they should not be confused with each other.

**Three items nobody can close by writing code** - listed just below. They are
the honest end of the original plan.

**Phase 22, at the bottom of this file, is a menu rather than a debt.** It came
out of comparing this game against its ancestor and its neighbours, and
nothing in it is required: the game is finished without any of it. It is there
so that "what next" has an answer that was thought about rather than picked on
the day.

- **Sound listened to on Windows and macOS** — listed twice, one thing. Everything
  a machine can check is checked: the synthesiser to the sample on every ground
  and in every machine, and now the device path on all three platforms. What is
  left is a person with speakers on a Windows machine and a person with speakers
  on a Mac, each saying it sounds right.
- **The transport has a written specification** — `docs/TRANSPORT.md` is written,
  the byte sizes it quotes are pinned by a test so it cannot drift from the code,
  and a client built from it completes a handshake with a real server. What is
  left is somebody who has **not read `gs_noise.c`** doing that, because the
  person who wrote the document wrote the code and cannot unsee it.

Everything else in this plan is done, and Phase 22 is a list of things that
would make it more rather than things that would make it whole.

## Phase 21 — Where the tests are not

Five places the suite does not reach, found by building the tree under a
coverage tool and asking what never runs. None of these is a known fault; they
are the places a fault could sit unnoticed.

- [x] **The server's own loop.** The 42% this item was written against was
      wrong, and wrong for a reason worth keeping: every test ended by *killing*
      the server, and a process killed outright records nothing for a coverage
      tool to read. Asked to stop instead, it measures 83% of its lines and 24
      of its 25 functions — twenty-six tests were driving it hard all along.
      What the killing hid was worse than a number: the server's shutdown path,
      which catches a signal and asks its own loop to finish, had never once
      run. A server that stopped stopping would have hung forever on somebody's
      machine with nothing to say so. And its dashboard — fifty-nine lines, the
      only interface it has — had never been drawn by anything, because drawing
      it needs a terminal and no test gave it one.
      *Verification: the server stops within a second and a half of being asked
      and is never forced; `--help` names every flag it has and exits; and given
      a terminal it draws its dashboard and repaints. The one function left is
      the once-a-minute heartbeat, which no test runs long enough to see, and it
      is named here rather than counted as covered.*
- [x] **Controls inside lists, at a small window.** The panels were walked
      exhaustively for reach at 640×480 and the controls drawn *inside* the
      lists on them were counted and skipped. They are walked now: the panel is
      scrolled to bring each list into view and then the list is scrolled
      through itself, which is what a person does.
      *Verification: 512 controls inside lists, on top of the 1935 on the panels
      themselves, every one of them wholly inside the box that holds it at some
      scroll position that box can be put at.*
      Three things had to be got right before the answer meant anything, and all
      three produced confident wrong answers first: a list is clipped by its
      panel as well as by itself, so the panel has to be moved first; a long
      list only draws the rows near where it is scrolled, so its contents cannot
      be listed once and then visited; and "wholly inside" cannot be asked of
      ImGui's own clip test, because a list's clip stops short of its own
      scrollbar and every full-width row overlaps it by a pixel.
- [x] **The server's library.** Measured across everything that uses it rather
      than one binary, it runs at 81% of its lines and 53 of its 54 functions —
      and the paths that could *leak* a track were already walked. The one that
      could **lose** one was not: opening a database made by an older build.
      Every test made a fresh one, so the migration that adds six columns and
      turns the old published flag into the new visibility had only ever run
      against a database that already had them, where it does nothing.
      *Verification: a database is built in the old shape, by hand, with two
      tracks in it — one published and one not — and opened by today's code. It
      comes forward rather than being refused, both tracks are still there with
      their bytes intact, the published one is still published and the private
      one still private, and opening it a second time is the same store rather
      than a second migration doing something else.*
      What is left under 80% is three SQL helpers whose remainder is error
      handling — a statement that will not prepare, a row that is not there —
      and that is named here rather than counted as covered.
- [x] **The rollback netcode over a bad link.** It was raced over exactly one
      link — 200 ms each way, 40 ms of jitter, one packet in eight gone — which
      says nothing about half a second, or a third of the packets, or no
      latency at all. Forty-eight links are raced now, and every one of them
      ends in the same world on both machines.
      **And the documented tolerance was wrong by nearly three times.** Every
      datagram repeats a quarter second of *inputs*, and the note over that
      number said a blackout shorter than a quarter second never needs asking
      for again. It does not follow: a quarter second of input history is worth
      nothing if the *promise* for one of those ticks never arrived, and the
      promises ride twelve times, not thirty-two. Eleven ticks of total silence
      is survived and twelve is not.
      *Verification: forty-eight links — four latencies, three jitters, four
      loss rates — all agreeing at the end; every blackout length from one to
      eleven absorbed without a retransmission; and every longer one stopping
      both machines at the same tick in the same world rather than letting them
      disagree, which is the half that makes the bound liveable.*
- [x] **The game played from the door to the results.** Everything else checked
      a piece — the unit tests drive the menu module, the render tests draw its
      screens, `play_check.py` races the real client. Nothing walked what a
      person does, end to end, in the real binary.
      *Verification: every screen the client can be asked for is opened and says
      so; a race is driven from the setup screen to the results; it leaves a
      time behind; and the game is then **opened again as a fresh process** and
      the time is still there on the screen that keeps times, along with the
      drivers and the library. That last step is the one worth having — a race
      that runs and a records screen that draws are both fine on their own while
      the time between them goes nowhere, and both ends of that path have broken
      before.*
      A session refuses to write anything, deliberately, so that a screenshot
      never touches a player's roster. It can now be told to keep what it did,
      which is consent given on a command line by something that has already
      pointed its preferences at a throwaway.
- [x] **The tracks the game ships reach a player who already has a library.**
      They did not. The stock tracks were read at start-up and the player's
      store was read afterwards, and reading a store replaces the library — so
      for everybody past their first ever run, every track the game shipped was
      thrown away before the menu appeared. A library built on the 19th was
      still all that a player saw a release later. The library is now reconciled
      with the shipped set after the store is read as well as before it, a track
      the game no longer ships is withdrawn, and the shelf holds sixty-four
      rather than thirty-two — twenty-four ship, and thirty-two left no room for
      the ones replacing what an older version shipped.
      *Verification: the game is started twice against two different shipped
      sets, with a store carried between them. The second start withdraws the
      track that stopped shipping, adds the one that started, and the player is
      left with the current set — not the set they were given the first time
      they ever ran the game. Removing the fix makes the check fail, which was
      confirmed by removing it.*
      A player's own tracks are never withdrawn, which is walked over all four
      combinations of whose a track is and whether the shipped set still names
      it, and an assets directory that reads as empty withdraws nothing rather
      than emptying somebody's library on a guess.
- [x] **Every gate faces the way the route goes through it.** A gate is a plane
      you cross, and its heading is which way through — so a gate turned across
      the route is one you drive along instead of through, with an arrow on the
      ground pointing where nobody goes. Every hand-written stock track gave
      every gate a heading of zero, and `the crossing`, which is a figure of
      eight, shipped with all four gates facing east. Validation now refuses a
      gate more than sixty degrees off the route, for loops and for paths, so
      the construction set catches it too.
      *Verification: the whole shipped set is walked rather than four files
      named — every track in `assets/tracks/` is validated, and the test states
      how many it found and how many it checked so a track added later is
      covered without anybody remembering. The rule itself is walked over both
      route kinds, every gate, and the angle right round the turn, and over all
      two hundred generator seeds.*
      The two tracks that were wrong were rewritten by deriving their headings
      from their own route, which moved their hashes; every other shipped track
      is byte for byte what it was.
- [x] **The way round is painted on the ground.** A gate's arrow says which way
      through that gate, and at racing zoom you see one of them at a time with
      no road edge in the window — which is a hint you have to have already
      understood rather than a route. A player read a gentle left-to-right
      sprint as two switchback turns, and nothing on the screen contradicted
      them. The route is now a dashed line along the ground the whole way,
      curved through the gates so it does not cut corners across the infield.
      *Verification: every leg of a four-gate loop is checked for the line at
      its midpoint, away from the gates where a waypoint post's own blue could
      otherwise answer for it, and the test states how many legs it found
      painted — so a route drawn on three sides of a square fails.*
- [x] **A minimap, the way the original had one.** The line on the ground says
      which way to go next; it does not say where you are on the track. Each
      view now carries a map in the corner the stats are not in: the whole
      route seen from above, the finish line across it, and every car as a dot
      in its own colour with yours ringed. The map and the ground line are the
      same curve, so what you steer by and what you read agree.
      *Verification: the route's blue is counted in the corner the map lives in
      — not over the whole frame, where the line on the ground would answer for
      it — and a track with no route on it leaves that corner empty, so the
      test cannot be passed by a panel that is always there.*
- [x] **A default track is a race rather than a demonstration.** Every track
      that shipped was a twenty-seven second drive — 28 to 173 tiles of route,
      averaging 63 — which had been asked about repeatedly and lost every time.
      The world was the reason: a field could be at most 64 by 64 tiles, and no
      route folded into that is longer than about five hundred tiles. The world
      is 192 now and the route is a serpentine of passes joined by half circles,
      so the eighteen tracks that ship are **997 to 1097 tiles — sixteen to
      seventeen times** what they replaced, or five to ten minutes of driving.
      The ten written by hand are on the same field, with their signature
      feature repeated across it so a track about a ramp has a ramp on every
      pass.
      *Verification: driven rather than measured — the AI gets round `bright
      run` in 4m 22s and `jupiter run` in 5m 15s, against twenty-seven seconds
      for what they replaced, and every track is raced by every vehicle from
      every grid slot. The suite measures the route of every track in the box
      and fails under the floor — it prints 997 to 1097 tiles against a floor of
      630 — so a set that shortens goes red on its own, rather than only the
      tool that writes them refusing to write a short one.*
      Three things moved with it, each found by something going red: the
      analyser gave every track ninety seconds and assumed a pace no car holds
      through a hairpin; a route of a thousand tiles needs a checkpoint every
      dozen rather than every hundred; and the generator is now understood to
      propose candidates, about one in twelve of which is thrown away by racing
      it rather than shipped.

- [x] **A track is a draw from a matrix, and thirty of them ship.** Every
      track used to be the same serpentine with different scenery. The
      generator now draws ten dials from the seed — circuit or path, three
      lengths, three curvinesses, three kinds of straight, three sizes of
      jump, four ground shapes at three severities, four gravities, three
      dressings and three road widths — and grows an organic route to satisfy
      the draw, so no two tracks fold the same way. The stock set is thirty
      tracks drawn from that matrix and none written by hand, each picked by
      racing it: every vehicle finishes from every grid slot and nobody is
      thrown off the world, with no excused exceptions left.
      *Verification: every dial is measured on the tracks it makes — a
      technical draw corners half as much again as a flowing one, every power
      straight is longer than any broken one, the severest ground out-hills
      the mildest moderate outright, and turning the jumps dial visibly
      changes the ramps. The tool refuses to write a set that misses a band
      of the matrix, and the suite races the whole shipped set with a full
      grid.*

- [x] **The tracks screen shows the shape of the chosen track.** The same
      picture the HUD's minimap draws mid-race — route, finish line,
      checkpoints — drawn beside the track's details, from one shared
      function so the two can never disagree. It takes only the room the
      detail fields leave over and disappears on a window too narrow for
      both, because the fields are controls and the picture is not.
      *Verification: the suite walks the preview's four states — beside a
      routed track, absent for a routeless one, absent with nothing chosen,
      and gone at the smallest window where the Copy button must stay whole
      instead.*

- [x] **The window opens where it was left.** Position and size are written
      to a small text file on the way out and restored on the way in — but
      only when the remembered spot is still on a display that exists, so a
      monitor unplugged since never leaves the game somewhere nobody can see
      or grab.
      *Verification: four consecutive real launches reopen at the same
      pixel; the suite refuses every kind of damaged memory file and walks
      twelve display arrangements, including the monitor that is gone.*

- [x] **The starting grid is centred on the line for the cars actually
      racing.** Two cars used to sit on one side of the start line - one in
      the middle, one at the edge - because slots were spread as if all four
      were taken. The pack is symmetric about the line now for any field
      size, at the same spacing as ever, and a full grid starts exactly
      where it always did so no recorded race moves.
      *Verification: the suite checks every field size from one to four for
      symmetry and spacing, and that the four-car grid is bit-identical to
      before.*

- [x] **High-speed cornering slides instead of ploughing.** Held steering at
      top speed used to turn the car at twenty-seven degrees a second on
      rails - reported as "completely unresponsive". Steering is half as
      much again on every machine and falls off less with speed; grip is
      untouched, so the surplus becomes a visible slide that scrubs speed,
      throttle holds it wide and brake tightens it. The AI learned to drive
      the new handling, a run-off physics fault it surfaced was fixed, and
      every golden hash moved deliberately with its note.
      *Verification: the slide is pinned with measured numbers - sixty-two
      degrees a second of nose against twenty-three of path, a third of top
      speed shed in two seconds of full lock, and the brake working
      mid-corner - and both suites are green over a shipped set re-raced
      under the new physics.*

- [x] **A window-focus blip no longer silently releases held keys.** Under
      WSLg the window loses focus in sub-second blips, and SDL forgets every
      held key when it does - so a held accelerator sometimes went dead
      mid-race until lifted and pressed again. Keys a driver never let go of
      now survive any focus loss shorter than a second; a longer absence
      drops everything rather than guessing.
      *Verification: the shield's rules are walked with forged events and
      forged clocks - the blip, the boundary, the real absence, and the
      release that always releases.*

- [x] **A lost car can be towed back to its last checkpoint.** Backspace
      (rebindable, pad button too) hands the car to the tow: it flashes
      where it was lost for a second, untouchable, then lands one tile
      before the last gate it crossed, solid and drivable - lost time is
      the whole cost. It rides in the input byte so replays, rollback and
      netplay all carry it; it gains nothing that was not already earned,
      does nothing before the first checkpoint, and leaves wrecks to their
      own flow.
      *Verification: refused on the grid, the hook held for its second on
      the nose, exact placement from mid-air, the same gate still owed,
      nothing gained by re-crossing, the flash pinned on counted pixels,
      and old control files load with the tow on its default key.*

- [x] **A held key that the keyboard stack delivers as flapping is still a
      held key.** Under WSLg a held arrow arrives as thirty press/release
      pairs a second, so the throttle read dead until re-pressed - caught
      in the act by the keystroke trace. A release now only counts after
      the key has stayed up for fifty milliseconds; a deliberate tap still
      releases. The tow's key also did nothing on release day - its action
      was missing from the action-to-bit table - and both the fix and a
      tripwire for the next missing entry are in.
      *Verification: the debounce walked against the flapping from the
      player's own log with forged clocks, and every action pressed on its
      real binding must resolve to its own bit, never silence.*

- [x] **Winning is something you can see happen.** Crossing the finish line
      used to move a number on a panel. Now the chequered flags wave,
      fireworks go up over the line, confetti falls across the view, and a
      banner says WINNER - in gold for a win - with your place under it.
      *Verification: the celebration is counted in pixels before and after a
      finish, it stops when the party is over, and the same tick drawn twice
      gives the identical frame - so a replay shows what happened. No golden
      hash moved: it is drawing, not simulation.*

- [x] **Checkpoints four times further apart.** Every gate used to be a
      checkpoint, a dozen tiles from the last, and a straight read as a
      fence of posts. Spacing the gates out broke the driver, the validator,
      the drawn route and the length measure at once, because the gates
      describe the road. So a track says how many gates make a checkpoint:
      the generator writes four, the editor has the dial, everything before
      counts every gate as it always did. Only checkpoints are marked; a
      missed waypoint is forgiven at the next gate.
      *Verification: driven past a waypoint and forgiven, past a checkpoint
      and stopped; an old file reads as it always did with the hash it had;
      a shipped track drawn both ways has a quarter of the marks. The
      generator golden moved; the track and world goldens did not.*

- [x] **Automatic and manual transmission.** Every machine has a forward
      gear count of its own - two for the rover, six for the sprint car -
      and one reverse. The box is picked per driver on the setup screen.
      An automatic drives exactly as the game always did; a manual gives
      you the shift, the limiter to shift off, and the bog of being left in
      too tall a gear. The engine is heard through the gears and the HUD
      shows the one you are in.
      *Verification: the gear ladder measured on its own numbers, the
      automatic driven to terminal speed and found in top, the manual
      shifting once per press and visibly slower off the line in the wrong
      gear. Online races still give everyone the automatic - the flag is
      not on the wire yet, and that is named in the status doc.*

## Phase 22 — What the neighbours have

Everything here came out of comparing the game against the one it descends
from and the others in its family, and each entry is argued for in
`FEATURES.md` rather than here. Nothing in this phase is required for the
game to be itself; it is the list of things a player who has met *Stunts*,
*Excitebike* or *TrackMania* would look for and not find.

**Ordered by what they give back for what they cost**, and that order is
the order they will be built in, each landing as its own commit with its
tests. Sizes are honest guesses: *small* is a session, *medium* a few,
*large* is a change to what something is rather than what it does.

| # | Item | Size | What it actually involves |
| --- | --- | --- | --- |
| 1 | Vertical drama | medium–large | New generator shapes, each measured; the set regenerated |
| 2 | Mirror mode | small | Reverse the gates, flip their headings; a dial on setup; its own records fall out of the hash |
| 3 | Split times | small–medium | The tick at each checkpoint kept in a ghost; the HUD shows ± at each post |
| 4 | A time to beat | small | The analyser already races every track; keep its best and show it |
| 5 | Overheating | medium | Heat in the car state - which moves the world goldens - a gauge, and power lost |
| 6 | A daily track | small | Date to seed; times compared through the server |
| 7 | Replay from any car | small–medium | Choosing the car in the replay viewer; nothing in the simulation |
| 8 | Alternate routes | large | A route that can be reached two ways is a change to what a route is |
| 9 | The central server | decided, then hosting | One hosted instance the game points at by default; see the item |

Mirror mode goes first in practice - it is the smallest item with the
largest payoff, since reversing a route doubles what there is to drive -
and vertical drama follows it, because it changes the shipped set and is
better done once the reversed routes are being verified too.

- [ ] **Vertical drama the ground can already hold.** Banked corners,
      quarter-pipe walls, half-pipes, gaps with nothing in them, ridges that
      drop away on the far side, craters that hold speed round the inside.
      The generator's terrain dials say how *rough* a track is and never what
      shape the roughness has, so none of these are ever asked for - and
      every one of them is a shape the ground can already take. This is where
      the vertical interest lives that loops were wanted for, at none of the
      cost.
      *Verification: each shape measured on the track it makes - a banked
      corner is one a car takes faster on the high line than the low, a gap
      is one that ends the race for a car that arrives too slowly - and the
      shipped set has to contain some of each, the way it already has to
      cover the matrix.*

- [ ] **Mirror mode.** Any track raced the other way round, as one dial on
      the setup screen. The route is directional, so this is a new track to
      drive for nearly nothing, with its own record.
      *Verification: the reversed route passes every check the forward one
      does - gates faced, finishable by every machine from every grid slot -
      and a record set backwards is never shown against a forward time.*

- [ ] **Split times, and where you actually lost it.** The gates are already
      sectors. Crossing one shows the difference against your own best for
      that sector, so a slower lap says *where* it went.
      *Verification: driven against a recorded ghost, the splits sum to the
      lap difference, and a sector nobody has driven before shows nothing
      rather than a wrong number.*

- [ ] **A time to beat on every track.** The analyser already drives every
      track with every machine before it ships, so the target is computed
      rather than authored. A target, never a medal: nothing unlocks.
      *Verification: every shipped track has one, it is beatable by a person
      on the machine it was set with, and it does not move when the track is
      loaded again.*

- [ ] **Overheating.** Sitting on the limiter builds heat, heat costs power,
      backing off spends it - which is what makes a manual gearbox a skill
      rather than a chore. Refused if it cannot be read at a glance.
      *Verification: shown on the HUD as it fills; a driver who never holds
      the limiter never sees it; and the power it costs is measurable rather
      than felt.*

- [ ] **A daily track everybody gets.** One seed a day, the same for
      everyone, with times compared through the server that already exists.
      *Verification: two machines asked on the same day build the identical
      track, and a day's times are only ever shown against that day's.*

- [ ] **Watching a race back from any car.** Replays already re-race
      exactly; what is missing is choosing whose shoulder to watch from. The
      same isometric view, following a car you pick - not a free camera,
      which stays out.
      *Verification: a replay watched from each car in turn shows the same
      race and the same finishing order.*

- [ ] **Alternate routes through a track.** The original's `whichway`
      offered seven; this generates one. A shortcut that is tighter, rougher
      or a jump you might not clear is a decision every lap. **The largest
      item here by a wide margin** - it needs a route that can be reached two
      ways, which is a change to what a route *is* rather than a change to
      what the generator draws with it.
      *Verification: a track with two ways round has both driven by the
      analyser, both finishable, and neither strictly faster than the other
      for every machine.*

- [ ] **The central server. Decided: one centralised server, not a mesh.**
      Publishing a track to a server and browsing what others built is done,
      and so are profiles with passwords and records re-raced before they
      count. What the feature asks for beyond that - accounts that follow
      you between machines and leaderboards that are global - follows from
      there being *one* server: a profile signed in there is the same profile
      on every machine, and its records are global because there is one place
      they live. Peer-to-peer discovery, relays between players' own servers
      and any federation are out: a second protocol to specify and secure,
      for a game whose whole defence against cheating is one place that
      re-races every time.
      What is left is not code so much as a place: a hosted instance, an
      address the game points at by default (today a player types one), and
      the running of it - **which cannot be finished, only kept up**.
      *Verification: a time set on one machine is shown, re-raced and
      verified on another with nothing copied by hand; and a service that is
      down leaves every local feature - racing, building, sharing by code -
      exactly as it was.*

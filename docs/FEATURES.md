# Features

**What this file is.** The menu, at the altitude of "what would the player
notice". Everything here came out of the design conversation, and it is written
so that a feature can be argued about, kept or dropped without anybody having to
read code. There is no implementation in this file on purpose — no data
structures, no formats, no function names. `COMPLETION_PLAN.md` is where a
feature turns into work with a verification attached; `PROJECT_STATUS.md` is
where it turns into a claim about what actually runs.

Each entry carries one of:

`CORE` — the game is not itself without this ·
`WANTED` — decided in, not yet scheduled ·
`CANDIDATE` — a good idea nobody has committed to ·
`OUT` — deliberately rejected, with the reason, so it does not get re-proposed

---

## The ethic

Gearstick is a modern *Racing Destruction Set* — Rick Koenig, Connie Goldman and
Dave Warhol at EA, 1985, C64 first. Not a remake of its content, a continuation
of its argument.

The argument is: **everything is a dial, nothing is locked, the model is simple
enough to predict and therefore exploit, and the chaos is the reward.** The game
handed you fourteen gravity settings, three surfaces, a two-car track editor and
no progression whatsoever, then got out of the way.

That gives a clean test for every feature below. A modernization that deepens
the dials, or hands the player a shorter loop between an idea and seeing it
happen, is in. One that adds fidelity at the cost of the player being able to
predict what the car will do is out, however impressive it looks.

---

## The original, kept

- **Isometric, both cars on one screen.** `CORE`
  A two-car collision has to be legible at a glance. This is the whole reason
  the view is what it is, and it constrains the camera work more than anything
  else on this list.

- **A construction set, not a track list.** `CORE`
  The editor is the game. Roughly half the total work, and it does not get
  deferred to the end — a racing game with an editor bolted on at the end is a
  racing game, which is not what this is.

- **Race mode and destruction mode.** `CORE`
  The same track and the same cars, one toggle: first past the flag, or last one
  driving.

- **Gravity as the headline dial.** `CORE`
  Moon to Jupiter. It changes jump distance, landing violence and the whole feel
  of a corner, and it is the setting people remember thirty years later.

- **Three surfaces — pavement, dirt, ice.** `CORE`
  Distinct enough to change the racing line, simple enough to hold in your head.

- **Vehicle choice as a set of trade-offs.** `CORE`
  Engine, tyres, mass. Boxy 1985 silhouettes: stock car, dune buggy, baja bug,
  sprint car, motorcycle, lunar rover.

- **Everything unlocked from the first second.** `CORE`
  No progression, no currency, no earning the good car.

- **Elevation, ramps and jumps as the track's third dimension.** `CORE`
  Airborne time is where gravity becomes visible, and the shadow under the car
  is what makes it readable.

- **Hazards you leave for the other driver.** `CORE`
  Oil slicks, mines and the rest of the destruction toolkit.

---

## Modernizations that amplify the original intent

- **Determinism as a product feature, not an implementation detail.** `CORE`
  A race is reproducible from its inputs. That single property is what makes
  ghosts, replay sharing, the editor's live ghost, headless track validation and
  rollback netcode possible at all — every one of them is downstream of it, so
  it comes first and everything else waits.

- **Rollback netcode for online play.** `WANTED`
  The honest 2026 translation of two people on one couch. The input is eight
  directions and a button, so the state is tiny and rollback is cheap.

- **Track sharing.** `WANTED`
  A track leaves the room as a short code or a URL. The 50-track floppy was a
  media limitation, not a design choice.

- **Instant test-drive from the editor cursor.** `WANTED`
  Drop in, drive, snap back to editing. No reload, no mode change worth
  noticing. The original's disk loads were the main thing standing between the
  player and the iteration loop, and removing them is the single biggest
  quality-of-life win available.

- **Continuous dials instead of discrete steps.** `WANTED`
  The fourteen gravity settings were a 6502 limitation. Make gravity continuous,
  and add air drag, friction scale and a damage multiplier beside it. Keep the
  planet names on the presets — that naming was doing real work, and "Jupiter"
  tells a player more than a number does.

- **Four-player split-screen, with the views merging into one when the cars are
  close.** `CANDIDATE`
  Fits the ethic exactly and the original could not have done it. The merge
  behaviour is the interesting part and also the risky part.

- **A headless mode.** `WANTED`
  The game runs a track with no window, at whatever speed the machine allows.
  Player-invisible, but it is what lets the editor ask questions about a track
  and lets CI notice that a change to the physics rewrote every existing time.

---

## New ideas worth building

- **Gravity as a brush, not a setting.** `WANTED`
  The best idea in the pile. Paint gravity onto the track per tile: a low-g
  pocket at the top of a jump, a Jupiter zone that pins you through a banked
  turn, a ramp whose landing sits under different physics from its take-off. It
  turns the most-loved feature of the original from a pre-race dropdown into a
  material you build with, and it is still a dial, still predictable, still
  exploitable.

- **A ghost that re-drives the track while you edit it.** `WANTED`
  A translucent car continuously re-racing the current design in the background.
  Raise a ramp, and two seconds later watch the ghost overshoot the landing.
  Track editing stops being blind construction and becomes a feedback loop.

- **Surfaces that change over the race.** `CANDIDATE`
  Dirt churns into ruts and loses grip on the line everyone is taking; ice
  polishes into something faster and looser; pavement does not care. Lap five
  stops being lap one, the ideal line moves *during* the race, and running
  second becomes a reason to pick a different line rather than a position to
  suffer.

- **Wreckage that stays.** `CANDIDATE`
  A destroyed car leaves debris that remains as track geometry for the rest of
  the race. Winning the fight reshapes the course.

- **A track analyser, shown as a heatmap over the editor.** `CANDIDATE`
  Sweep the track across gravity values and vehicle choices and report the
  envelope: completable between these two gravities in these six vehicles, that
  third jump impossible below 0.9g, this corner is where the time goes. Nobody
  in 1985 could tell you whether their track was any good.

- **A landing-prediction arc, off by default.** `CANDIDATE`
  A dotted trajectory to the predicted touchdown point while airborne. It reads
  like a modern hand-hold and is the opposite: it makes the physics model
  legible, and legibility is exactly what lets a skilled player exploit it.

- **Tracks identified by their content.** `WANTED`
  Two players with the same track have the same track, without a server
  deciding so; editing one cleanly produces a new one. Ghosts and times then
  aggregate by themselves.

---

## Deliberately not

- **Tyre slip curves, suspension travel, weight transfer.** `OUT`
  The moment the model stops being mentally simulable, the exploit-the-physics
  joy dies. The car should be predictable in your head, not accurate on a rig.

- **Progression, unlocks, currency, a career.** `OUT`
  RDS handed you everything on load. Anything that withholds a dial is working
  against the premise.

- **A chase camera, or a free camera.** `OUT`
  Isometric is what makes a two-car collision readable. A camera behind one car
  makes the other car a surprise.

- **Collisions that punish rather than launch.** `OUT`
  Modern impulse resolution is fine; the tuning is not. A hit should send
  somebody somewhere funny, not quietly cost them two seconds.

- **Shipping the original's tracks or art.** `OUT`
  The 50 stock tracks and the C64 sprites are EA's. They are worth studying —
  the manual's designer notes explain the *intent* behind tracks like "headon"
  and "destruct" — and worth measuring the isometric angle against. Calibrate
  with them; ship our own.

---

## Open questions

- **Sprites or geometry for the vehicles.** The authentic path is pre-rendered
  rotations; the other path draws the same models live and sidesteps the
  combinatorics of heading × pitch × roll × damage entirely. Cheap to defer, and
  the answer probably arrives the first time the sprite count is written down.

- **How far the destruction toolkit goes.** The original's hazards are the
  floor. Whether there is anything beyond dropped hazards — and whether it stays
  inside "chaos is the reward" — is undecided.

- **Whether surface wear and persistent wreckage are one system or two.** Both
  change the track mid-race. They may want to be the same idea.

- **What the merged four-player camera does when it cannot merge.** The failure
  mode is the design, and it has not been thought about yet.

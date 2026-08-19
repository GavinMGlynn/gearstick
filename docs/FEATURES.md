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
  else on this list. The *view* is fixed and isometric; what it looks at is
  real three-dimensional ground and real three-dimensional cars, which is what
  lets a car lean into a banked turn and nose up a ramp rather than snapping
  between a fixed set of poses.

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

- **A ground for every world on the dial.** `WANTED`
  The gravity dial already names eight bodies, and eight worlds that all look and
  drive like a car park is a missed opportunity sitting in plain sight. Dust that
  is loose and almost frictionless under a sixth of a gravity, sand that will not
  let you accelerate, basalt that grips like nothing else and punishes a landing,
  slush that drags. **Each one has to change how the car behaves or it is a
  colour swatch**, so every surface earns its place in the same three numbers the
  first three use — grip, rolling resistance, how much engine reaches the ground
  — and in what it turns into once it has been driven on.
  The gas giants have no ground at all, which is worth being honest about: their
  terrain is one of their moons'.

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

## The platform

The game is a thing you install and play. This is the layer around it that
remembers who you are, keeps what you have built, and lets it travel — decided
in as a direction, and deliberately kept at arm's length from the racing itself.

- **Drivers, not player slots.** `WANTED`
  A name, a colour and a favourite machine, remembered between sessions, with a
  history behind it. "Best lap 0:42.1" is a number; "Ada, 0:42.1, baja bug" is
  something to beat. Everything else here hangs off this.

- **Records that mean something.** `WANTED`
  Best lap and best race per track — and a record is a time *on a track under
  conditions*, so a lap set at a sixth of gravity is not a lap. Change the
  dials and it is honestly a different table rather than a leaderboard nobody
  can read.

- **Everything remembered between runs.** `WANTED`
  Drivers, records, and the tracks you have built. Nothing that took an evening
  to make should live only in the window it was made in.

- **A track library rather than a save file.** `WANTED`
  Your tracks are a collection you can browse, not a filename you have to
  remember. Tracks identified by content means the library needs no naming
  authority: the same track from two people is the same entry.

- **Share a track with somebody, or publish it to everybody.** `WANTED`
  Sharing already works with no server at all — a track is a few hundred
  characters you can paste into a message. Publishing is the other half: put it
  somewhere people can find it, with who made it and what has been done on it.

- **A central service for the things a single machine cannot do.** `CANDIDATE`
  Accounts that follow you between machines, a browsable library of what
  everybody has built, and global leaderboards. This is the one part of the
  project that cannot be "finished" — it is hosting, moderation and cost, and
  it should be entered into deliberately rather than drifted into.

- **Times verified by re-racing them.** `CORE`
  A submitted time comes with the inputs that produced it, and a server can
  re-race those inputs and check the answer. Cheating reduces to "drive that
  fast", which is the only leaderboard worth having. This is possible *because*
  the simulation is exactly reproducible, and is the strongest argument for
  having built it that way.

- **A relay for people whose routers will not cooperate.** `WANTED`
  Two players who can reach each other should race each other directly, because
  that is the fastest path and the game is about response. A relay exists for
  the people who cannot, and forwards packets without ever simulating anything.

- **Racing stays between the players.** `CORE`
  The service is a librarian and a referee, never a player. A race simulated on
  a server means every steering input waits a round trip, and for a game where
  you are making continuous small corrections that is the difference between
  feeling good and feeling broken. The service checks races afterwards; it does
  not run them.

- **A track belongs to somebody, and the ones that shipped belong to nobody.**
  `WANTED`
  Whoever built a track can change it, take it down, keep it to themselves, hand
  it to a few named people, or put it up for everybody. The tracks that came with
  the game are outside all of that: nobody can edit or delete them, because a
  library whose furniture can be taken away is a library that eventually has
  nothing in it. Content addressing does half the work already — an "edit" of
  somebody else's track is a new track with a new identity, so nothing can be
  altered underneath anyone even by accident.

- **A profile you can prove is yours.** `CANDIDATE`
  The moment a name owns published work and a place on a leaderboard, it is worth
  taking, and a name typed at a prompt proves nothing. A password at least, and a
  second factor from a phone for anybody who wants one.
  **The hard part is not the second factor.** A one-time code is a small,
  dependency-free piece of arithmetic; what makes this real work is that the
  protocol has no confidentiality at all today, so a password would cross the
  wire in the clear and a code could be replayed by anyone on the path for the
  half minute it lives. The answer is to never send either: the server offers a
  challenge and the client returns a proof over it. That is a smaller change than
  encrypting everything and it is the part worth designing carefully.

- **The library never reaches into a race.** `CORE`
  What is stored decides *which* race to run and records what happened, and
  nothing in between. Anything a race read from a local database would be a
  thing two machines could disagree about, and disagreeing is the one failure
  this design cannot absorb.

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

- **How far the destruction toolkit goes.** The original's hazards are the
  floor. Whether there is anything beyond dropped hazards — and whether it stays
  inside "chaos is the reward" — is undecided.

- **Whether surface wear and persistent wreckage are one system or two.** Both
  change the track mid-race. They may want to be the same idea.

- **What the merged four-player camera does when it cannot merge.** The failure
  mode is the design, and it has not been thought about yet.

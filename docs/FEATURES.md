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
`DONE` — built, and ticked in `COMPLETION_PLAN.md` with its verification ·
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

- **A ground for every world on the dial.** `DONE`
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

- **Automatic and manual transmission.** `DONE`
  A gearbox as a choice, not a difficulty setting: automatic shifts for you,
  manual gives you the shift as one more thing to be good at. How many forward
  gears a machine has - and its one reverse - is a property of the vehicle,
  like its power and its tyres. The engine's sound climbs and drops through the
  gears, which is how a driver hears the race, and the HUD shows the gear you
  are in.

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

- **Rollback netcode for online play.** `DONE`
  The honest 2026 translation of two people on one couch. The input is eight
  directions and a button, so the state is tiny and rollback is cheap.

- **Track sharing.** `DONE`
  A track leaves the room as a short code or a URL. The 50-track floppy was a
  media limitation, not a design choice.

- **Instant test-drive from the editor cursor.** `DONE`
  Drop in, drive, snap back to editing. No reload, no mode change worth
  noticing. The original's disk loads were the main thing standing between the
  player and the iteration loop, and removing them is the single biggest
  quality-of-life win available.

- **Continuous dials instead of discrete steps.** `DONE`
  The fourteen gravity settings were a 6502 limitation. Make gravity continuous,
  and add air drag, friction scale and a damage multiplier beside it. Keep the
  planet names on the presets — that naming was doing real work, and "Jupiter"
  tells a player more than a number does.

- **Four-player split-screen, with the views merging into one when the cars are
  close.** `DONE`
  Fits the ethic exactly and the original could not have done it. The merge
  behaviour is the interesting part and also the risky part.

- **A headless mode.** `DONE`
  The game runs a track with no window, at whatever speed the machine allows.
  Player-invisible, but it is what lets the editor ask questions about a track
  and lets CI notice that a change to the physics rewrote every existing time.

---

## New ideas worth building

- **Gravity as a brush, not a setting.** `DONE`
  The best idea in the pile. Paint gravity onto the track per tile: a low-g
  pocket at the top of a jump, a Jupiter zone that pins you through a banked
  turn, a ramp whose landing sits under different physics from its take-off. It
  turns the most-loved feature of the original from a pre-race dropdown into a
  material you build with, and it is still a dial, still predictable, still
  exploitable.

- **A ghost that re-drives the track while you edit it.** `DONE`
  A translucent car continuously re-racing the current design in the background.
  Raise a ramp, and two seconds later watch the ghost overshoot the landing.
  Track editing stops being blind construction and becomes a feedback loop.

- **Opponents you can actually race, with their smarts on a dial.** `DONE`
  There is a driver in here already - it plans a line from the grip it has here
  and now rather than following a baked one, so it re-thinks the corner the
  moment somebody moves the gravity dial. What there is no way to do is *race*
  it: every car in a race takes its input from a pad, and the AI drives only in
  headless self-play, the editor's ghost and the demo attract mode. A one-player
  race is a lone car going round on its own.
  So: fill the empty grid slots with opponents, and put their skill on a dial
  like everything else - one continuous setting from a driver who brakes far too
  early to one who is faster than you are, rather than three named presets. The
  dial has to change how they *drive* and not just how fast they go: where they
  brake, how much they will lean on a surface they do not trust, whether they
  take a jump flat. And it has to hold up on the tracks people build, which is
  the harder half - the current driver, put on a bare rectangle with two gates,
  laps it twice and then sits in the run-off for as long as you leave it.

- **Surfaces that change over the race.** `DONE`
  Dirt churns into ruts and loses grip on the line everyone is taking; ice
  polishes into something faster and looser; pavement does not care. Lap five
  stops being lap one, the ideal line moves *during* the race, and running
  second becomes a reason to pick a different line rather than a position to
  suffer.

- **Wreckage that stays.** `DONE`
  A destroyed car leaves debris that remains as track geometry for the rest of
  the race. Winning the fight reshapes the course.

- **A track analyser, shown as a heatmap over the editor.** `DONE`
  Sweep the track across gravity values and vehicle choices and report the
  envelope: completable between these two gravities in these six vehicles, that
  third jump impossible below 0.9g, this corner is where the time goes. Nobody
  in 1985 could tell you whether their track was any good.

- **A landing-prediction arc, off by default.** `DONE`
  A dotted trajectory to the predicted touchdown point while airborne. It reads
  like a modern hand-hold and is the opposite: it makes the physics model
  legible, and legibility is exactly what lets a skilled player exploit it.

- **Tracks identified by their content.** `DONE`
  Two players with the same track have the same track, without a server
  deciding so; editing one cleanly produces a new one. Ghosts and times then
  aggregate by themselves.

---

## The platform

The game is a thing you install and play. This is the layer around it that
remembers who you are, keeps what you have built, and lets it travel — decided
in as a direction, and deliberately kept at arm's length from the racing itself.

- **Drivers, not player slots.** `DONE`
  A name, a colour and a favourite machine, remembered between sessions, with a
  history behind it. "Best lap 0:42.1" is a number; "Ada, 0:42.1, baja bug" is
  something to beat. Everything else here hangs off this.

- **Records that mean something.** `DONE`
  Best lap and best race per track — and a record is a time *on a track under
  conditions*, so a lap set at a sixth of gravity is not a lap. Change the
  dials and it is honestly a different table rather than a leaderboard nobody
  can read.

- **Everything remembered between runs.** `DONE`
  Drivers, records, and the tracks you have built. Nothing that took an evening
  to make should live only in the window it was made in.

- **A track library rather than a save file.** `DONE`
  Your tracks are a collection you can browse, not a filename you have to
  remember. Tracks identified by content means the library needs no naming
  authority: the same track from two people is the same entry.

- **Share a track with somebody, or publish it to everybody.** `DONE`
  Sharing already works with no server at all — a track is a few hundred
  characters you can paste into a message. Publishing is the other half: put it
  somewhere people can find it, with who made it and what has been done on it.

- **A central service for the things a single machine cannot do.** `WANTED`
  Accounts that follow you between machines, a browsable library of what
  everybody has built, and global leaderboards. This is the one part of the
  project that cannot be "finished" — it is hosting, moderation and cost, and
  it should be entered into deliberately rather than drifted into.
  **Decided: one central track server, not a mesh.** The server that exists
  — a meeting point with a library, profiles and re-raced records — becomes
  *the* server: one hosted instance the game points at by default, so a
  profile signed in there is the same profile on every machine and its
  records are global because there is one place they live. Peer-to-peer
  discovery, relays between players' own servers and any kind of federation
  are `OUT`: a mesh is a second protocol to specify, secure and debug, for a
  game whose whole cheating defence is one place that re-races every time.
  **And the server gets a window.** It has drawn its live view in a terminal
  and run on machines with no screen, and it keeps doing both; where there
  is a display it shows the same facts in a window, with a log the terminal
  scrolls away and the operator's controls beside it.

- **Times verified by re-racing them.** `CORE`
  A submitted time comes with the inputs that produced it, and a server can
  re-race those inputs and check the answer. Cheating reduces to "drive that
  fast", which is the only leaderboard worth having. This is possible *because*
  the simulation is exactly reproducible, and is the strongest argument for
  having built it that way.

- **A recording knows who drove it.** `DONE`
  Re-racing proves a time was driven. It does not prove *you* drove it, and a
  recording that does not name its driver is a thing anybody who obtains one can
  hand in as their own. The driver belongs inside the recording, and the server
  checks it against whoever is submitting.

- **The whole race is checked, not just the winner's lap.** `DONE`
  Everybody keeps the full input log and the final state hash they all agreed
  on. Re-racing the log has to produce that hash, or one of the machines in that
  race was not running this game. Almost free, for the same reason as everything
  else here.

- **Nobody sees anybody else's inputs before committing their own.** `DONE`
  Rollback hands every machine the others' inputs for a tick, which means a
  modified one can wait and then decide. Nothing desyncs — everybody simulates
  the dishonest choice faithfully — so the state-hash check that catches a
  changed *simulation* cannot see it. The answer is to commit to a choice before
  anybody else's is visible: send a hash of your inputs, then the inputs.

- **The parsers are fuzzed.** `DONE`
  Everything the server acts on arrived from somebody who may be hostile. The
  protocol decoder, the reassembler and the deserialisers behind them are the
  most likely place in this program for a memory-safety bug, and well-formed
  input is the only thing they have ever been given.

- **A written specification for the transport, and a written threat model.**
  `DONE`
  What is defended, from whom, what is deliberately not defended, and enough
  detail that somebody who has not read the code could write a client. Both are
  deliverables in their own right: a design nobody wrote down cannot be
  reviewed, and the parts left out are exactly what a reviewer most needs to see
  were decided rather than missed.

- **A relay for people whose routers will not cooperate.** `DONE`
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
  `DONE`
  Whoever built a track can change it, take it down, keep it to themselves, hand
  it to a few named people, or put it up for everybody. The tracks that came with
  the game are outside all of that: nobody can edit or delete them, because a
  library whose furniture can be taken away is a library that eventually has
  nothing in it. Content addressing does half the work already — an "edit" of
  somebody else's track is a new track with a new identity, so nothing can be
  altered underneath anyone even by accident.

- **Nothing on the wire in the clear.** `DONE`
  Every datagram between a client and the server, and between two players,
  sealed — so what crosses somebody's network is not readable by them and cannot
  be altered on the way. The server's key is its identity, pinned the first time
  you meet it the way an SSH host key is; there are no certificates and nothing
  to expire.
  **Nothing here is invented.** A hand-rolled handshake is the thing that fails
  review, and it fails it for good reasons. This is a named pattern from a
  specified framework, built on somebody else's audited primitives, and it is
  held to that by running the framework's own published test vectors and by
  talking to an independent implementation of the same pattern. A protocol whose
  only evidence is that its author believes in it is not evidence.
  **It has to be sealed one datagram at a time**, because the racing tolerates
  loss and reordering and a design that recovers a stream would turn a dropped
  packet into a stall. That also means replay protection is a sliding window
  rather than a counter, or ordinary reordering would look like an attack.
  Worth being clear about what it does not do: it cannot stop somebody cheating
  in a game running on their own machine. Re-racing a submitted time is what
  handles that. This stops the different problem of being watched or interfered
  with in transit — and it is what makes a password possible at all.

- **A profile you can prove is yours.** `DONE`
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

## From the neighbours

Everything below came out of comparing this game against the one it descends
from and against the others in its family — *Stunts* (1990), *Excitebike*
(1984), *Rock'n'Roll Racing* (1993), *Super Off Road* (1989), *Championship
Sprint*, *Stunt Car Racer* (1989), *Micro Machines*, *TrackMania* (2003) and
*Trials*. Most of what those games have, this one already has or has
deliberately refused. What is here is the residue: things a neighbour does
that this game has no answer to, and that survive the test at the top of this
file.

- **Mirror mode.** `DONE`
  Race any track the other way round. The route is directional — the gates
  face — so reversing it is one dial that doubles what there is to drive and
  makes a track you know into one you do not. Nothing is generated, nothing is
  unlocked, and the record for a track backwards is its own record. This is
  the cheapest content in the file by a wide margin.

- **Split times, and where you actually lost it.** `DONE`
  The gates are already sectors. Crossing one against your own best says
  **+0.4** or **−0.2** for that sector, so "I was slower" becomes "I was
  slower *there*" — which is the difference between knowing you lost and
  knowing what to practise. *TrackMania* and *Trials* both live on this and it
  costs a comparison against a ghost that is already recorded.

- **A time to beat on every track.** `DONE`
  The analyser already drives every track with every machine before it ships,
  so a target time is a number the game can work out rather than one somebody
  has to author. Shown as a target, not a medal to earn: nothing unlocks,
  nothing is withheld, it is only the difference between a solo lap with a
  point and a solo lap without one.

- **Overheating.** `DONE`
  *Excitebike*'s gauge, and the manual gearbox gives it a reason to exist:
  sitting on the limiter builds heat, heat costs power, and backing off or
  changing up spends it. It makes the shift matter for a reason other than
  acceleration, and it is a dial a player can see filling. Refused if it
  cannot be made legible at a glance — a hidden number that quietly takes your
  engine away is the opposite of predictable.

- **Alternate routes through a track.** `CANDIDATE`
  The original's `whichway` offered "seven different routes" and this game
  generates exactly one. A shortcut that is tighter, rougher or a jump you
  might not clear is a decision every lap rather than a line to memorise —
  which is the same argument the gravity dial wins on. The hard part is not
  drawing it: it is a route model where a gate can be reached two ways, and
  that is a real change rather than a tweak.

- **A daily track everybody gets.** `CANDIDATE`
  A generated track is a seed, and there is a server. One seed a day that
  everybody races and compares times on costs almost nothing and gives a
  reason to open the game on a day you had not planned to build anything.

- **Vertical drama the ground can already hold.** `DONE`
  The ambition behind wanting loops, without the geometry that makes them
  impossible here. Banked corners you lean into and can take faster for
  committing to the high line. Quarter-pipe walls at the edge of a bowl that
  you ride up and come down off. Half-pipes to cross rim to rim. Gaps with
  nothing in them, where the question is whether you arrive fast enough.
  Ridges that drop away on the far side so the landing is the decision.
  Craters and hollows that hold a car's speed round the inside of them.

  Every one of those is a shape the ground can take today and the generator
  hardly ever asks for — its terrain dials say how *rough* a track is and not
  what shape the roughness has. This is where the vertical interest actually
  lives, and none of it costs a change to how the world is built.

  Two of the neighbours are worth measuring against here rather than copying:
  *Stunts* got its drama from pieces bolted together, and *Trials* gets all of
  its from a single plane seen side-on. The lesson from both is that the drama
  is in **what the ground does under a committed line**, not in how many
  directions the track can point at once.

- **The flat moments.** `CANDIDATE`
  Crossing the line is a moment now - flags, fireworks, a banner - and three
  others still pass without one: the green light, a collision, and landing a
  big jump. Each is presentation derived from the simulation, never state in
  it, which is what keeps every replay and every machine agreeing.

- **The setup online: gearbox and direction.** `WANTED`
  A race hosted online gives everyone the automatic and races forward,
  because neither choice travels with the race setup. They should - the
  gearbox per driver, the direction per race.

- **Watching a race back from any car.** `CANDIDATE`
  Replays already re-race exactly; what is missing is choosing whose shoulder
  to watch from. *Stunts* shipped a replay viewer in 1990 and it is half the
  fun of a crash. Not a free camera — that stays out, below — the same
  isometric view, following a car you pick.

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

- **Loops, corkscrews, bridges and overpasses.** `OUT`
  *Stunts* had all four and they are the one structural thing this game cannot
  answer: the ground is a field of heights, one per point, and a bridge needs
  two. Changing that means giving up the property that makes terrain editing
  simple to think about and correct by construction — arbitrary elevation
  joins with no tile set to stitch. Named here so it is a decision on the
  record rather than a gap somebody rediscovers.

- **Progressive damage that changes the handling.** `OUT`
  *Stunt Car Racer* did this well and it fails the test at the top of this
  file: a car that steers differently because of history you cannot see is a
  car you cannot predict. Damage stays a number that ends your race when it
  runs out, which you can watch approaching.

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

- ~~**What the merged four-player camera does when it cannot merge.**~~
  *Decided.* It splits one pane per player, and every pane is the same size and
  fills its share of the screen. Three players used to get the four-player grid
  with a quarter of the window blank; they get three columns. What was
  considered and not taken: panes that hold whichever cars happen to be together,
  so a race would run with two panes, then three, then two again. That is a
  screen whose shape a player cannot predict, and predictability is the whole
  ethic.

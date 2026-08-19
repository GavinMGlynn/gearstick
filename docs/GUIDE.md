# Gearstick — the player's guide

A construction racer. You build a track, you drive it, and the two are the same
activity. If you played *Racing Destruction Set* on a C64 in 1985 you already
know what this is; if you did not, the short version is that the interesting
part was never the driving.

Everything below has been done in the order it is written. If a step does not
work, that is a bug and the last section says how to report it in a form
somebody else can run.

---

## The first ten minutes

**1. Start it.** Run `gearstick`. You land on the title screen with a track
already turning behind it — that is a real track, not a picture of one.

**2. Say who you are.** Press **Drivers**. Type a name, pick a colour, pick a
machine, press **add**. This matters more than it looks: every lap time you set
from now on has your name on it, and the game remembers you between runs.

**3. Race.** Back out with **Back**, then **RACE**. The setup screen has the
whole race on it:

- *mode* — **first past the flag** or **last one driving**. The second is the
  same track, the same cars and the same physics; only the winning condition
  changes. That was true in 1985 too.
- *laps* — how long.
- *drivers* — one to four.
- *gravity* — eight presets from Ceres to Jupiter. This is not a difficulty
  slider. On the Moon you fly much further and stop much later; on Jupiter you
  barely leave the ground and everything hurts.
- *the grid* — who is driving what, in what colour.

Press **GO**.

**4. Drive.** Arrow keys. Up is the accelerator, down is the brake, left and
right steer. Player two is **WASD**. Watch the shadow under your car — the gap
between the car and its shadow is the only thing telling you how high you are,
which is exactly how it worked on a C64.

**5. Look at the results.** Times for everybody, best laps, and whether anybody
took a record. **Records** shows what stands on this track.

**6. Now build something.** Press **Tab**. This is the point of the game.

---

## Driving

| | Player 1 | Player 2 |
| --- | --- | --- |
| accelerate | Up | W |
| brake / reverse | Down | S |
| steer left | Left | A |
| steer right | Right | D |
| drop a hazard | Right Shift | Left Shift |

**Players three and four need a gamepad**, or a rebind — there is no sensible
third and fourth set of keys on one keyboard, so there is not a bad one either.
Plug in a pad and it is picked up; to change any of it, open the construction
set and press **controls...**, then click a control and press what you want.

Other keys, any time:

| | |
| --- | --- |
| **Tab** | in and out of the construction set |
| **Escape** | back one screen; quits from the title |
| **R** | restart the race |
| **H** | show or hide the ghost of your last run |
| **G** | show painted gravity as colour on the ground |
| **M** | music on or off |
| **F5** / **F9** | save the last run as a ghost file / load one |

### Things worth knowing

**The grip circle is real.** A tyre has one budget and cornering and
accelerating spend the same one. Braking in a straight line then turning is
faster than doing both at once, and that is a physical fact here rather than a
tuning value.

**Landing flat hurts.** What damages a car is not how fast it was falling but
the *mismatch* between how fast it is coming down and how fast the ground is
falling away underneath. Land on a downslope going downhill and it barely
registers. Land the same jump on the flat and it folds a sprint car. This is the
whole reason to build a downhill landing.

**A wall is a wall.** Ground steeper than about fifty degrees cannot be driven
up. You stop.

**The machines are genuinely different.** Each of the six is the best at
something and hopeless at something else — that is checked by a sweep in CI
rather than hoped for. The sprint car is quickest on pavement and useless on
rough ground. The lunar rover is slowest everywhere and the only thing that gets
anywhere at all on Ceres. The baja bug wins nothing on speed and survives a
staircase of drops that wrecks everything else.

---

## The construction set

Press **Tab**. The car stops where it is; nothing is loaded and nothing is
saved.

**Painting.** Pick a brush, then click and drag on the ground.

- **raise** / **lower** — terrain, by the *step* in tiles. Held down, it is one
  undo step, not four hundred.
- **surface** — pavement, dirt or ice.
- **gravity** — paint a patch of ground that pulls differently. Press **G** to
  see it: violet where it pulls less, amber where it pulls more.
- **gate** — the route. Gate one is the start and the finish.

**radius** is how wide the brush is, **step** is how much it moves per stroke.

**Camera.** Arrow keys pan. The mouse points at the ground.

**undo** / **redo** — the whole history, and it really is the whole history.

**Test-drive it now.** Press **Tab** again and a car drops in where you were
looking, facing the way the start line does. Press **Tab** to come back and the
track is exactly as you left it, undo history and all. This loop — build, drive,
build — is the single biggest thing thirty years of hardware buys over the
original, where every one of those steps was a disk load.

**The live ghost** is on by default: a car continuously re-racing your design in
the background. Draw a ramp in its path and it is somewhere else entirely on its
next lap. Nothing tells it the track changed; it notices.

**analyse** runs every machine round your track at nine gravities and tells you
what can actually get round it. It takes a few seconds and it is worth it — the
question "is this driveable at all" is one no amount of staring answers. Tick
**heatmap** and the ground shows you where everybody actually went, which is
rarely the line you drew.

**Route** is checked continuously and tells you what is wrong with it. A race
will not start on an unsound route.

**save** / **load** keeps one track in your preferences directory.

**copy code** puts the whole track on your clipboard as a few hundred characters
of text. Paste it into a message and somebody else presses **paste code** and
has your track — no server, no upload, no file. If the code is damaged in
transit it is refused rather than quietly becoming a track nobody built.

---

## Racing other people

**On one keyboard**, set *drivers* to two on the setup screen. Three and four
need pads.

**Online**, one person hosts and the others join:

```sh
gearstick --host 47000 4          # wait for four players in total
gearstick --join their-address 47000
```

Nobody starts until everybody has arrived.

**Or through a server.** `gearstick_server` is a separate program that acts as
the meeting point — you all connect to it instead of to each other, which
matters when somebody's router will not accept an incoming connection. It shows
who is there and how far away they are:

```sh
gearstick_server --port 47800 --players 4
```

Then everybody else meets there:

```sh
gearstick --server their-address 47800 --name ada
```

You get a lobby screen showing who has arrived and how many are still missing.
**Which player you are is the server's decision**, not a matter of who started
first.

The server does not run the race. Your machine does, and so does everybody
else's — that is why your car responds to your steering immediately instead of
waiting for the network. The server holds the lobby, and later the track library
and the records. The netcode is rollback: your car
responds to your steering on the frame you press it, and when the game guesses
wrong about what somebody else did it quietly rewinds and replays. You will see
another car twitch occasionally. You will not wait for the network to see your
own car move.

Everyone must be on the same track. Right now that means the same build, since
there is no track transfer in the handshake yet — see *What is missing*.

---

## Ghosts

Finish a race and your run becomes the ghost for the next one automatically —
one keypress, no files. **H** hides it.

**F5** writes that run out; **F9** loads one back; `--ghost FILE` starts a race
against somebody else's. A ghost is not a recording of positions: it is the
inputs somebody actually pressed, re-raced by your machine through the same
physics. That is why it is half a kilobyte a second, and why it is exactly the
run they drove.

A ghost recorded on a different track is refused rather than raced.

---

## Reporting a bug so somebody else can run it

This is the part that makes a bug fixable, and the game is built so it is easy.

**If it is about a track**, press **copy code** in the construction set and
paste the code into the report. That is the whole track, exactly.

**If it is about a race**, press **F5** and attach the ghost file. It carries
the conditions, the machines, the grid and every input — replaying it on another
machine produces the same race, tick for tick.

**If the game itself seems wrong**, run:

```sh
gearstick_cli selftest --verify
```

It re-races a fixed nine-hundred-tick race and checks it lands exactly where it
should. If that fails, say so first: it means the simulation on your machine
disagrees with the simulation everywhere else, which is a much more interesting
bug than whatever you were about to report.

Include what you were doing, what you expected and what happened. A code or a
ghost file turns "the car went through the ramp" into something anybody can
reproduce in ten seconds.

---

## What is missing

Said here rather than discovered:

- **There is one track.** The construction set can build any track and save one;
  there is no library to browse yet.
- **A server race still needs everybody on the same track**, because the
  handshake does not send it yet — the same limitation as `--host`/`--join`.
- **Online needs everybody on the same track already.** The handshake does not
  send the track.
- **Only players one and two have keyboard controls** by default.
- **Sound has been listened to on Linux.** Windows and macOS have not been.

`docs/COMPLETION_PLAN.md` is the list of what is done and what is not, with the
check that was actually run against each item. `docs/PROJECT_STATUS.md` is the
honest version of what works.

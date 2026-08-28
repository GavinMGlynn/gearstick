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

### The track analyser, and the wall it found

The analyser races all six vehicles round a track at nine gravities, from 0.15x
Earth to 2.55x, and reports the band the track is actually driveable in along
with a heatmap of everywhere anybody went. It is on a button in the editor and
on `gearstick_cli analyse FILE`, and it is deliberately not continuous: a sweep
is fifty-four thirty-second races, which is a very long frame.

What it is *for* is the question no amount of looking at a track answers — can
this be got round, by what, and under what gravity — and the first thing it did
was fail. Asked to confirm that a sixty-tile wall across the track was
impassable, it said the track was fine. It was right and the physics was wrong:
a grounded car followed the terrain height whatever the terrain height did, so
a vertical face demanded an enormous upward velocity and got one. The traced car
climbed the wall and was flung to six hundred tiles up.

Ground steeper than a gradient of 1.2 is now a wall rather than a ramp, and a
car that meets one stops. That is worth more than the analyser is: it is a class
of nonsense that any player could have found by driving at a cliff, and nothing
in the suite had ever asked.

It cost two retunes. The roster's washboard was spikes of 1.9 tiles over one
tile of run — which is to say a row of walls, ridden over only because the
physics permitted climbing walls. It now rises over two tiles and falls over
two, keeping the amplitude the suspension feels while leaving a gradient a car
can climb. That flipped "rough and twisty" from the dune buggy to the stock car
by half a percent, so the amplitude went up until grip decided it properly: the
buggy now wins by nearly half.

And the baja bug lost its niche outright, because its old one had been an
artifact of the bug. Its distinguishing stat is toughness, so its condition is
now a staircase of shelves — forty seconds of landing flat, over and over. The
sprint car, the motorcycle and the stock car are all wrecked by it; the dune
buggy wrecks too; the baja bug finishes it undamaged, and beats the only other
survivor by half again. All six vehicles win something, and each of them wins it
for a reason you could describe to somebody.

### Ghosts, and the grid a replay was not carrying

A ghost is a recorded run stepped through the same simulation as the live race,
one tick for one tick. It is not an animation and holds no positions: the same
input log that makes a replay makes a ghost, at half a kilobyte a second, and
the car you are chasing is the car somebody actually drove rather than a video
of where it went.

Finishing a race hands the recording straight to the ghost, so racing yourself
costs one keypress and no files. `F5` writes that run out, `F9` reads one back,
and `--ghost FILE` starts against somebody else's - which is refused outright if
it was recorded on a different track, because the same inputs somewhere else are
a different race and pretending otherwise would put a car through the scenery
and call it a lap time. A borrowed ghost survives restarting; your own is
replaced by every run you finish, which is the point of it.

Building it exposed that replays were not self-contained. Version 1 stored the
conditions, the vehicles and the inputs, and left the starting positions to the
caller - fine while the only thing replaying a race was the program that
recorded it, and useless the moment somebody sends you one. The format is
version 2 and carries the grid: position and heading per car. `gs_replay_playback`
now needs nothing from its caller but somewhere to put the answer.

The verification runs on every platform in CI, inside `selftest --verify`, and it
is deliberately stronger than the plan asked for. The recording goes out to the
wire format and back, and the ghost is then required to agree with a fresh run
of the same race at *every one* of the nine hundred ticks - not merely at the
end. A ghost that agrees only at the end is one that drifts and then arrives,
which is exactly the ghost nobody can race against.

### Sharing a track, and what a run-length coder could not see

A track leaves the room as about seven hundred characters of URL-safe text -
`copy code` in the editor puts it on the clipboard, `paste code` takes one back,
and `gearstick_cli code`, `url` and `decode` do the same from a terminal. There
is no server, no account and no upload: the track *is* the message.

The first packer was PackBits, and it was nearly useless here for a reason worth
recording. A track is mostly flat identical ground - but surface and gravity are
interleaved per tile, so flat identical ground is not a run of one byte, it is a
*pattern* of two, and a run-length coder sees no runs in it at all. It saved an
eighth. LZSS with a four kilobyte window sees the period and saves nine tenths:
a 4,009 byte track became 3,515 characters, and now becomes 704. That is the
difference between a code you can paste into a message and one you cannot. The
worst case - a full 64x64 track with every corner randomised - packs in 5.6 ms,
which is fine for a button and would not have been for a frame.

The code carries the track's own content hash, and decoding rebuilds the track
and checks it. A code that lost a character usually still decodes to *something*,
and that something must never be handed over as a track somebody built. Every
single-character change to a real code is either refused or decodes to the
identical track; the ones that are accepted differ in bytes the track format
does not read, which is slack in the format rather than in the check.

Because a code is a wire format between two people rather than inside one
program, it is pinned like the golden replay: a small fixed track and the exact
string it encodes to are committed in the tests. It is byte-identical across
gcc -O0 through -Os and clang, and clean under UBSan and ASan.

### Rollback, and two bugs that both looked like something else

Nothing is sent but inputs. Each machine runs the whole race and guesses what
the other player is doing - they are usually still holding what they were
holding a moment ago - and when the truth arrives a few ticks later, the machine
rewinds to where the guess went wrong and replays. Nobody ever waits for a
packet before their own car moves.

All of it is downstream of two decisions made long before it: the simulation is
deterministic, so replaying the same inputs from the same state lands in the
same place on both machines; and the world has no pointers, so the snapshot to
rewind to is a memcpy of 8,776 bytes. `src/core/gs_net.c` links nothing and has
never heard of a socket - it produces and consumes byte arrays - which is what
lets a twelve-second race under 200 ms of latency, 40 ms of jitter and twelve
percent packet loss run inside a unit test, deterministically, with no network.
That race ends with both machines confirming all 1,440 ticks, agreeing on the
state hash, and agreeing with the same race run on one machine with no network
at all. The last of those is the one that says the rollback is *correct* rather
than merely consistent.

Two bugs, and neither looked like what it was.

The input history is a ring of 256 ticks, and slots were never cleared when
reused - so tick 256 read tick 0's inputs as known truth. It does not present as
a ring bug. It presents as a desync four seconds into an otherwise perfect race.
Slots now carry the tick they hold.

The second was worse, because everything still worked. Prediction searched back
only as far as the confirmed tick - and the confirmed tick is precisely the one
whose remote input has not arrived, so the search found nothing and predicted an
idle player. On a race where both players simply held the accelerator, that
guessed wrong on every tick and rolled back on every tick: 584 rollbacks in 600
ticks, 8,760 ticks of wasted re-simulation, and a race that was correct
throughout. Rollback was firing on agreement. Predicting from the whole retained
history took the steady-input case to one rollback - the unavoidable first, when
nothing at all is known - and the bad-connection race from 737 to 61.

The socket is SDL_net, a new submodule under `ext/`, and it is used for
datagrams and nothing else: every comfort a stream offers is a round trip, and
rollback exists so that nobody waits one. A separate test runs a real race over
real loopback sockets, because the one thing a simulated link cannot check is
whether the socket works - whether a host finds a peer it was never told about,
and whether the ends still agree once real datagrams carry the race.

**Four players, not two.** The session was N-player from the start - the input
history, the known-input bitmask and the confirmed-tick logic never cared how
many there were - so what four needed was peer discovery and a mesh rather than
a netcode rewrite. `--host PORT [N]` waits for N in total and `--join HOST PORT`
joins; nobody races until everybody has arrived, because the grid depends on the
count and rollback can recover from a wrong guess about an input and from
nothing at all about a wrong starting state.

It is a **mesh, not a star**: every machine sends directly to the other three,
because relaying through the host would put the host's latency between two
clients who can see each other, and latency is the entire thing this design
exists to minimise. The host is used only to *find* everybody - joiners knock
until answered, it assigns the slots, and it hands each of them a personalised
roster saying which player they are and where the others live. After that it is
nobody's server.

One detail worth writing down: the roster's entry for the host is sent **empty**
on purpose. A host does not know what its own address looks like from outside -
behind a router it is not the one it is bound to - and every joiner already
knows it, because they typed it. So they fill that entry in themselves. The
honest limitation is the other side of the same coin: a joiner's address as the
host sees it is the one other joiners are given, which is correct on a LAN and
is not NAT traversal.

Four machines racing over twelve simulated links, all with different latencies
and ten percent loss, confirm every tick, agree on the hash, and agree with the
race one machine would have run. Four real processes on the loopback meet, take
distinct slots and start. A fifth is given silence rather than a reply telling
it there is a game here.

Desync is detected rather than lived with: every packet carries the sender's
confirmed tick and the hash of its state there, and the receiver checks it
against its own. A desync that is noticed is a bug report. One that is not is
two people describing different races to each other.

### The art pipeline, and getting the proportions wrong twice

Cars were boxes. They are now meshes, and every one of them is generated from a
parameter table in `tools/make_meshes.py` rather than modelled by hand or
downloaded from anywhere. Six vehicles, 488 vertices, 732 triangles, and no
third-party art in the game at all - which means there is no licence condition
to satisfy, which is a better answer to the licence question than any amount of
careful attribution.

It is not a compromise at this fidelity. What reads at this size is the
silhouette, and six vehicles that must be distinguishable at a glance and
consistent with each other is exactly the job a parameter table does better than
a person with a mouse. Changing every car's ride height is a number here, not an
afternoon. `--showroom` parks the whole line-up so a change can be looked at
rather than described.

Colours are not baked. Each triangle carries a *role* - body, trim, glass, tyre,
metal, light - and the renderer decides what a role looks like, which is what
lets four players share one mesh and a wreck darken without a second set of
geometry. Faces are culled in screen space and the survivors sorted back to
front, because a car is a union of boxes and boxes are not convex together.

The cars also lean now. The simulation has no pitch or roll and does not need
any, but the terrain has a slope, so the renderer builds a basis from the
heading tilted by the ground underneath and plants the car on it. A car on a
ramp points up the ramp.

The proportions were wrong twice, and both times it was the only thing anybody
would have noticed. First pass: wheels three quarters of the car's height, so
every vehicle read as a monster truck. Second pass: still half again too big.
The fix was to stop guessing and write the real ratio down - a saloon is
4.5 m long on 0.65 m wheels, which is 1 : 0.145 - and scale that to the 1.3
tiles a car is drawn at. That comment is now in the tool, because it is the
thing most likely to be got wrong again.

Attribution is written in the same run as the art, into `assets/ATTRIBUTION.md`.
A licence statement that can drift from the art it describes is worse than none,
and the only way to be sure it never drifts is for one command to produce both.
CI runs the generator twice and diffs, so the committed art cannot drift from
what the tool writes and the tool cannot be accidentally non-reproducible.

A test caught itself being useless here, which is worth recording: the check
that a car leans on a slope compared a frame on a ramp against a frame on flat
ground, and passed with the lean disabled - because the *ground* differed. It
now renders the same ramp twice, grounded and airborne, and compares only the
car's own pixels.

### Sound, and a test that measured the noise instead of the note

Synthesised, not sampled - for the same reason the art is generated, and for a
better one: a sample is a recording of one engine at one speed, and what a race
needs is a note that follows a drivetrain continuously from idle to the limiter
and back down through a change. That is a synthesiser's job.

There is a real gearbox behind it. Five ratios and a final drive, with
hysteresis on both changes so a car sitting at a shift point does not hunt.
Without a gearbox the pitch is a straight function of road speed and the car
sounds like a vacuum cleaner accelerating; with one, the note climbs, drops, and
climbs again, and a player can hear how fast they are going without looking.
Tyres are noise through a one-pole filter coloured by the surface - dirt
rumbles, ice hisses - scaled by how sideways the car is. Impacts are noticed
rather than reported: damage only ever goes up, so a jump in it is a collision
or a landing that hurt, and the size of the jump is how hard. And a car in the
air loses its tyre noise entirely while the engine goes light, because nothing
is loading it. That last one is the sound everybody remembers.

Two bugs, both found by measurement rather than by listening.

The tyre filter changed the *level* as well as the tone. A one-pole low pass fed
white noise puts out sqrt(k / (2 - k)) of what went in, so the darkest setting
is also the quietest - and dirt at full tyre gain came out quieter than ice at
half of it. The surface was reaching the synthesiser perfectly and arriving
backwards. Compensating for the filter's own gain fixed it.

The other was in the test. "The engine note follows the drivetrain" was checked
by counting zero crossings, which counts the broadband tyre noise sitting on top
of the note rather than the note - so it failed while the engine was working
perfectly. Replacing it with a normalised autocorrelation took two more goes:
the unnormalised kind reported the sixth multiple of the true period at speed,
and even an unbiased peak-pick lands on a subharmonic about half the time. It
now reads 28.4, 49.0, 98.0 and 149.5 Hz across four speeds, stable run to run
and matching the gearbox model exactly - and the test asserts the thing that
matters: seven tiles a second is 3.5x the speed of two, and the note is only
1.5x higher, because the car changed up twice on the way.

The first mix peaked at a tenth of full scale, which is a race you turn the
amplifier up for and then get deafened by the next thing you play. It now peaks
around 0.43 with a soft knee above that, and a test walks every sample of four
cars landing on each other at full volume to confirm nothing ever leaves the
range a speaker cone lives in.

`--audio-out FILE.wav` writes a race out to listen to, one tick of audio per
tick of race, which is how the plan's "listened to" verification gets done at
all. It has been listened to on Linux; **Windows and macOS have not been
listened to yet** - the synthesiser is platform-independent but the device path
is not, and claiming otherwise would be claiming a check nobody has run.

### Music, seeded by the track itself

Composed rather than recorded, and **the seed is the track's own content hash**.
A track already carries an identity - the number that says two people who built
the same thing built the same thing - so that number is handed to the composer
and every track gets its own tune. Build a ramp and the chorus changes; undo it
and the old one comes back. Nobody writes fifty pieces of music for fifty
tracks, and nobody hears the same one on all of them.

The instruments are a 1985 machine's: pulse waves with a moving duty cycle, a
triangle, and noise for percussion. That is not nostalgia for its own sake -
three simple oscillators are what a generative score can be genuinely good at,
because what carries a chiptune is the composition and the arpeggios rather than
the timbre. The chords are one voice playing three notes very fast, which is how
they were done when one voice was what there was.

The composer picks a key, a mode, a tempo between 112 and 168, and one of four
progressions, then writes out eight bars: a bass on the beat with passing notes
where a bass player would put them, a lead over it that thins into a turnaround
every fourth bar, and percussion that stays out of the way. All of it decided
once, when the track loads; what runs per sample is a sequencer reading tables,
which is the division a tracker made in 1985 and for the same reason.

It is deterministic and tested for it: the same seed is the same music sample
for sample, and two hashes one bit apart give different pieces - not just
different samples but different keys and tempos, checked across two dozen seeds.

The music mixes into the same buffer as the race, before the soft clip rather
than after. Mixing it in afterwards - which is how it was written first - lets
it push the total past full scale on exactly the loud moments the clip exists
for.

One test needed rewriting for the usual reason. "The music goes somewhere rather
than repeating one bar" compared rendered audio a few seconds apart, and passed
when the arrangement was forced to bar zero forever - because the oscillators
drift, so a one-bar loop renders differently anyway. It now walks the chord of
each bar directly and requires the progression to actually visit several.

### The front end, and the store behind it

Title, drivers, race setup, results, records. A session now starts from a cold
launch, chooses who is playing and what they are driving, races, and comes back
to a table of times - with no command line anywhere in it, which was the whole
requirement and is a bigger one than it sounds.

The setup screen shows the standing lap record *before* the race rather than
after, because the number you are driving at is more use than the number you
missed. It also refuses to start on a track whose route is unsound, and says to
go and fix it in the construction set rather than starting a race nobody can
finish.

Behind it, three things that had to exist first:

**A race that can end.** The simulation gained a lap target and a finish tick,
and the race is over when nobody is left who could still finish - not when the
first car crosses, because everybody's time is what a results screen is for.
Laps are timed in the simulation and hashed like everything else: a best lap is
what a track is actually judged by, so it is state two machines have to agree
about rather than something a front end watches for and works out afterwards.

**Records.** Keyed by the track's content hash and by a hash of the dials,
because a lap set at a sixth of gravity is not a lap. One row per driver per
track per conditions per distance - the distance is part of the key, and without
it driving a longer race quietly deleted the shorter record. A test caught
exactly that, which is the second time this week a test has caught a key that
was one field short.

**Drivers.** A name, a colour, a favourite machine and a history. None of it
touches the simulation: a colour cannot change where a car ends up, so it is not
in the hash, and somebody can repaint without invalidating a replay or a record.

All of it is one file in the preferences directory, written when something
changes and read at startup, so a second run of the game knows who you are and
what you have done.

**The look** is dougbinks' "dark clear", ported from the collection pinned at
`ext/imgui_styles`. What is worth taking from it is not the colours but the
idea: there is *one* palette, written light, and "dark" is a transformation of
it rather than a second palette to keep in step with the first. Invert the value
of every colour with almost no saturation - the greys, the backgrounds, the text
- and leave the saturated ones alone, so the blue accent comes through unchanged
while everything around it turns inside out. "Clear" then scales the alpha of
anything already translucent, which is what puts the track visibly behind the
menu instead of merely behind it.

The layout is not theirs: the reference is a debug overlay on a voxel editor,
and a title screen is not. The front end gets room to breathe, aligned label
columns, real tables for the grid and the results, and exactly one loud button
per screen. The construction set keeps the same colours at a tool's density and
much less transparency - a brush palette is read while the thing underneath it
is being changed, and a see-through panel over moving terrain is one you squint
at.

One thing had to be given up. The panels obviously want to size themselves to
their contents, and they cannot: an auto-fitting ImGui window is invisible for
its first frame, and a screenshot is one frame, so the whole front end would
photograph as an empty screen. That cost an afternoon once already on the
editor's palette. The screens with tables in them compute their height from the
number of rows instead, which gets the same result and can be captured. `--session` runs a whole race by itself and stops on the
results, which is how the verification is checked rather than asserted: two
drivers, a real race, a real table, and the records still there when a second
process starts cold.

### The player's guide, and a test that keeps it honest

`docs/GUIDE.md`: the first ten minutes as a path somebody can walk, every
control, the construction set, racing other people, and - the part that earns
its place - how to report a bug as a file somebody else can run.

That last section is short because the game was built so it could be. A track is
a few hundred characters on the clipboard, so "here is the track" is a paste. A
race is its inputs, so `F5` produces a file that re-races identically on
anybody's machine. And `gearstick_cli selftest --verify` re-races a fixed
nine-hundred-tick race, so "the simulation on your machine disagrees with
everybody else's" is a thing a reporter can find out before they write anything.

**A guide that has drifted from the code is worse than no guide** - it sends
somebody to press a key that does nothing and lets them conclude the game is
broken - and nothing in a build catches that. So a test reads the guide as
shipped and checks its control table against the bindings the game actually
starts with, in both directions: changing a default binding fails the test, and
so does renaming a command the guide tells people to type. Both were confirmed
by breaking them.

The plan's verification is "someone else follows it and races", and a person has
not. What has been done is to follow it mechanically from a genuinely cold start
- no store file, nothing remembered - through the title, a full race, a results
table, records that survive into a second process, a track through the clipboard
and back byte-identical, a ghost written and read, and the selftest. Every claim
in it was checked against the code, including the ones that are easy to write
and wrong: the eight gravity presets, that all six machines win something, and
that fifty degrees is where a slope becomes a wall.

### Releases: unsigned, and provably built here

Tagging publishes a GitHub release with all three platforms in it, a combined
`SHA256SUMS`, and a **build provenance attestation** on every file.

The releases are not code-signed, and `docs/RELEASES.md` is a whole document
about that rather than a footnote - because "just click through the warning" is
terrible advice that people give constantly. It says exactly what SmartScreen
and Gatekeeper will say, exactly which click gets past each of them, and what to
do about macOS's *"is damaged and can't be opened"*, which is the quarantine
attribute rather than damage.

The reasoning is worth stating plainly: code signing means paying Microsoft's
and Apple's certificate authorities a few hundred dollars a year so that a name
is attached to the binary. That says who paid. It says nothing about what went
in. A provenance attestation is a signed statement by GitHub's own
infrastructure that this exact file came out of this workflow from this commit,
it is free, and anybody can check it with one command. It is the stronger claim,
so that is what is shipped - and the document tells the person downloading how
to run the check rather than assuming they will not.

**The verification is the interesting part.** "Run on a machine that never had a
toolchain on it" is not something a CI runner can honestly claim about itself:
the runner has a compiler, and a package can quietly depend on something only a
build machine has. So the Linux job now unpacks the release inside a bare
`debian:12-slim` container that has no compiler, no CMake and no SDL, refuses to
proceed if it finds one, and runs the packaged game there - the headless driver
re-racing the golden replay and the game drawing a frame.

Locally, where Docker was not reachable, the same claim was checked the other
way round: the packaged `gearstick_cli` links nothing but libc, and the packaged
game links nothing but libc, libm and the GCC runtime. SDL is static. There is
nothing in the package that a toolchain would have provided.

One more test earns its place here, and it earned it by being wrong first. The
release notes describe the files people download, and a rename in CPack would
silently turn that into a description of files nobody has. The first version
checked that the notes contained the strings the notes contained - a test that
can only pass - and it duly passed while the notes named
`gearstick-VERSION-windows-x86_64.zip`, which the packaging has never produced.
It now asks the build what it calls a package on this platform and looks for
that, and CI runs it on all three.

### The server, and two tests that proved nothing

`gearstick_server` listens, holds a lobby of up to four, and draws a live view
of who is there — name, address, round trip, datagrams each way — with the
totals underneath. It runs headless: SDL is initialised with no subsystems at
all, because SDL_net needs SDL and not a display, and a server that demanded one
could not run where servers run.

The protocol is `src/net/gs_proto.c`, which links nothing — no sockets, no SDL,
no allocation. The same discipline the rollback session keeps and for the same
reason: a protocol that can be exercised without a network is one whose edge
cases can be tested at all.

Two decisions worth writing down. A refusal carries a reason, because a client
turned away has to tell its user why and "connection failed" is not why. And a
client is matched on address *and* port, because two people behind one router
share an address and are not the same player.

**Two of the first tests here passed while the thing they checked was broken**,
which is worth more than the code they were testing.

The first checked that a fifth client is refused — on a server configured to
hold four. Deleting the capacity check entirely did not fail it, because with
four allowed and four seats there is nothing to observe. It now starts a server
told to hold *two* and watches the third bounce, which is the only arrangement
where the setting being used and the setting being ignored look different.

The second checked that saying hello twice does not take two slots — at the end
of the four-player test, with every seat taken. There, a server that duplicated
a rejoin and a server that *refused* one both leave the count at four. It is now
its own test with room to spare, and it checks the rejoining client gets the
same slot back rather than merely that the count did not move.

Both were found by breaking the server and watching the tests stay green, which
is the whole reason that step exists.

The ping column had the same shape of problem and was caught the same way: the
server displayed a round trip it never measured. Only the end that sends a ping
can time its own reply, so a server that wants to show one has to ask — it now
does, every two seconds, and a test fails if the asking stops.

### The lobby, and a bug the test had to be sharpened twice to catch

`gearstick --server HOST PORT --name ada` meets everybody at a server instead of
at each other, and **which player you are is the server's decision** rather than
the decision of whoever started the game first. That is the difference between
a lobby and a host. There is a screen for it: who is here, which one is you, how
many are still missing, and a refusal shown in words meant for a person rather
than as a failure to connect.

The same `gs_wire` either way. Once everybody is found, sending and receiving
work identically, so nothing above that layer knows or cares how the players
met.

**The bug worth recording** was mine and it was subtle. The server's address was
kept in slot zero of the peer table — and then the roster arrives and fills the
peer table with players, overwriting slot zero with *player zero's* address. Any
client not placed first lost the server completely and was dropped for silence
a few seconds later. The server now lives outside the peer table, which is what
the comment claiming slot zero "is the server rather than a player" should have
meant.

Finding it took sharpening the test twice, which is the part worth keeping:

The first version watched a placed client for six seconds against a fifteen
second patience, so nothing could die inside the test. The server gained a
`--timeout` flag purely so a test can reach the behaviour — a timeout nothing
can reach is a timeout nothing tests.

The second version then passed while the client stopped reading entirely,
because the client's own heartbeat kept the server happy on its own. Staying
connected is only half of it: a client that had stopped listening would sit on a
roster from six seconds ago and never notice the person it is racing had left.
The test now closes one of the two and requires the other to notice, and
breaking the read path fails it.

One smaller thing, found by looking at a screenshot: a lobby that has not heard
from the server yet was drawn as a table of nobody under "waiting for 0 more
players". Nothing heard is not an empty lobby, and it says "Knocking..." now.

### The track travels with the race

`gearstick_server --track FILE` hands the track out. It goes in chunks, because
a track is a few kilobytes and a datagram is not, and **the hash is the whole
reassembly protocol**: every chunk carries the hash of the track it belongs to,
and the rebuilt track has to hash to it before anybody races. A track already
knows what it is, so nothing needs a transfer id or a session, and two transfers
cannot be confused with each other.

Loss is handled the way everything else here handles it — by asking again rather
than by acknowledging. The receiver knows which pieces are missing because it
knows how many there are, so it asks for the track again and gets all of it; a
chunk that arrives twice is written twice to the same place and costs nothing.

The client is **not ready to race until the ground is agreed**, and the last
part of getting that right is the part worth recording.

The first version inferred "there is no track to wait for" from the server not
having mentioned one. That is inferring a fact from silence, and it was wrong in
a way no amount of loopback testing would show: the roster and the track
announcement are two datagrams, the roster usually arrives first, and in the gap
the client believed there was nothing to wait for and was ready to race on
whatever it had loaded locally. The server now *always* says what the race is
on, including when the answer is "nothing", and a client that has not heard yet
counts as not settled.

The test took three goes to catch that, and the shape of the failure is the
lesson. Waiting for the transfer to finish and then checking it proved nothing,
because on the loopback the track lands milliseconds after the roster — a client
that ignored the transfer entirely looked identical by the time anybody looked.
What is checked now is every intermediate state and the rule that has to hold in
all of them: never ready while the ground is still arriving.

A damaged track is refused rather than raced. All the pieces arriving is not the
same as the track being right, and two machines racing on tracks they each
believe are the same one is the one failure rollback cannot absorb — every input
would agree and every state would differ.

### The relay, and a poll that ate the race

`--relay` sends everything through the server instead of to the other players.
It is a last resort and a real one: peers that can reach each other should race
each other directly, because that is the shortest path and this game is about
response — but a meaningful number of home connections will not accept anything
unsolicited, and for those the choice is a relay or no game.

**The server forwards without understanding.** The payload is a rollback
datagram; the server stamps who it came from and passes it on. A server that
parsed race traffic would be a server that could disagree with the race, and the
whole design rests on it never being able to.

The bug was mine and it was a good one. `gs_wire_poll` used to return early once
everybody was found, and making it keep running — so a connection could maintain
itself — left it draining every datagram and throwing away whatever was not
control traffic. A relayed race therefore delivered *nothing*: every forwarded
packet was swallowed by the poll that was supposed to be idle. Poll now drains
only while still finding people; once a race is running the caller's own receive
loop is the pump, and control traffic is handled on the way past.

One test detail worth keeping. The first version fired seven hundred datagrams
in a few microseconds and failed — and it was right to, but not for the reason
it looked. The netcode absorbs loss by design; what it cannot absorb is a kernel
socket buffer overflowing because nothing paced the sender. A race sends one
packet per player per tick at 120 Hz, so the test does too, and the server's
drain loop sleeps a millisecond rather than five.

### What the server remembers

SQLite, **on the server only**. The client's store is the same versioned flat
file it has always been, the game links no SQLite, and `gearstick_cli` still
links nothing but libc. It is obtained as the single-file amalgamation, pinned
by SHA-256 and fetched once at configure time, so there is no package manager,
no submodule and no Tcl — and a system copy is used instead when there is one.

Three tables, and the shape of each is decided by what it is:

**Drivers** by name, because a name is what a record carries. One row however
often somebody appears; a repaint updates it rather than adding another.

**Records** keyed by track, conditions, distance *and* driver — all four. That
is not normalisation pedantry: a lap set at a sixth of gravity is not a lap, a
three-lap time is not a five-lap time, and dropping either from the key is the
difference between a leaderboard and a mess. The better-time-wins rule is in the
statement rather than done by reading first and writing after, so two results
arriving together cannot lose one of them.

**Tracks** content-addressed. Two people uploading the thing they both built are
storing the same thing, which is what a content hash is *for* — the schema's
primary key is what enforces it, and a test confirms that is load-bearing rather
than decorative.

Every statement is prepared with bound parameters, never assembled. A driver's
name arrives over a network from somebody the server has never met, and there is
exactly one way to be safe about that. The test for it is a driver called
`'); DROP TABLE driver;--`, who races, sets a record, and is still just somebody
with an unusual name afterwards.

The operational payoff is real: the server's state is a `.db` file that
`sqlite3` opens.

**A time is offered rather than proven, for now.** The server believes what a
client tells it. The next item has it re-race the inputs before accepting
anything, and the message does not change when that happens — only what the
server does with it.

### Times verified by re-racing them

**The strongest thing this project's determinism buys.** A claimed time arrives
with the inputs that produced it, the server re-races them through the same
simulation the player used, and the time is kept only if the replay produces it.

That reduces cheating to "produce an input log that genuinely drives that fast",
which is not cheating — it is being good at the game. It works only because a
race is exactly reproducible from its inputs, which is the property everything
in `src/core/` is arranged around, and it is the best argument for having built
it that way.

What is checked is not equality but *betterness*. A claim faster than the inputs
produced is rejected; a slower one is accepted, because somebody being wrong in
their own disfavour costs them the record they did not take and nothing else.
The track, the distance and the dials are all checked too, and all three come
out of the recording rather than out of the claim — a lap driven on the Moon
cannot pay for a claim about Earth, however the claim is worded.

**The bug here was a good one and it was about size.** The carrier that
reassembles a track was reused for the proof, and it was sized from the track
format: about twenty kilobytes. A replay is one byte per car per tick, and a
three-lap race is tens of thousands of ticks — so every proof was truncated, the
chunks past the end were refused, the transfer never completed, and an honest
time was never verified at all. It is now sized from the largest thing that
travels that way rather than from the first thing that did, and derived from the
replay format rather than typed in.

The claim and its proof travel separately and the server holds the first until
it has the second. A claim that arrives with no proof is not a record: silence
is not evidence, and a test sends one to make sure it stays that way.

### The library

Tracks are a collection rather than a save slot: up to thirty-two of them, kept
by content hash, with a name and an author beside each. Stored in the same file
as the drivers and the records, because they are one thing — a record with a
name on it is only a record if the name still means somebody, and it is only a
record of anything if the track is still there.

**A name sits beside a track rather than identifying it.** Rename a track and it
is the same track, which is what anybody would expect and what an id-keyed
library gets wrong. Storing the same track twice stores it once, and two people
who built the same thing have the same entry — that is what content addressing
is *for*.

Editing is the interesting case, because a changed track has a changed hash and
is honestly a different track. `gs_library_replace` exists for when that is not
what the person meant: they were working on *this* one and the slot should
follow, keeping its name. Editing a track into one already in the library leaves
one entry rather than two of the same thing.

The stored hash is never read back from the file — it is derived from the track
that was actually stored. A library whose recorded hash disagreed with its
recorded track would be a library that lies about what it holds, and the track
is the thing that is there.

The store is version 2 and refuses a version 1 file rather than half-reading it.
That is correct and it is also the first real reason to build the migration path
the tails have been asking for.

### Choosing a track

A **Tracks** screen: everything you have built, with its author, which one is
loaded, and a name you can change. Load one and it becomes the track a race
happens on; keep the one you are editing; forget one you are done with.

Two details worth recording, both of which were wrong first and both of which
were caught by looking at a screenshot rather than by a test.

The rename box was empty over a track that had a name, because it was filled in
when a row was *clicked* and the selection could also be set without a click. It
now watches the selection instead of relying on every place that changes it
remembering to refresh — a field that must be updated at every such place is a
field that is eventually stale at one of them.

And the buttons were off the bottom of the panel, because the height was
computed for a shorter screen than the one that got built.

**Loading a track throws away the undo history**, and that is correctness rather
than tidiness. The edit log records a cell changing from one value to another,
and those values belong to the track that was being edited; undoing after a load
would apply one track's edits to another. `gs_edit_reset` exists for that and a
test walks it: undo after a load does nothing at all, rather than reaching into
the new track.

### The demo track is gone

It had been in the frontend since Phase 3, and it was a prototype the day it was
written. **A track in C is a track nobody can edit, share or replace.**

Four stock tracks now ship as data in `assets/tracks/`, written by
`tools/make_tracks.c` — first light, the long drop, ice house, jupiter run. Each
one is a shape with a reason: a ramp with room either side to learn what the car
does; a shelf that ends, so the landing decides the run; an ice sheet where grip
is the whole problem; and a painted low-gravity pocket over a jump, which is the
thing this game has that the original could not. The game loads all of them into
the library at startup, so a fresh install has four tracks and a returning
player keeps whatever else they built.

`grep` for track geometry in `src/frontend/` now returns nothing. Even the
fallback used when no assets are found states none: `gs_track_init` zeroes every
corner, so flat ground is what it already is rather than something the frontend
writes out.

All four are completable between 0.15x and 2.55x Earth gravity, checked with the
analyser rather than by driving them once and hoping. A test reads them as
installed and requires a sound route, at least two gates, and some elevation — a
stock track with no elevation would mean the tool wrote nothing and nobody
looked.

Two things this turned up. The assets directory has no trailing separator, so
the first version looked in a path with the directory name run into the
subdirectory and quietly found nothing — the fallback made that look like a
design decision rather than a bug. And **the server was not in the install rules
at all**, while `docs/RELEASES.md` already listed it as something a release
contains. A document describing a file nobody has is worse than no document.

### Publishing

A client sends a track up and asks for it to be listed; anybody else can browse
what is published, fetch one and race it; and whoever put a track up can take it
down again.

**Published is a separate thing from stored**, and that distinction is the
design. The server holds every track it has ever been handed, because it needs
them to verify times set on them — publishing is somebody saying "and let people
have this one". Withdrawing stops it being listed and leaves the track exactly
where it is, so a record set on it stays checkable long after its author lost
interest in showing it off.

Only whoever put a track up can take it down. That check has a SQL wrinkle worth
recording: `sqlite3_changes()` counts writing a value that is already there as a
change, so a withdrawal of something already down reported success. The `WHERE`
requires `published <> 0` now, and a test withdraws twice.

The track travels the way tracks always travel — in chunks, checked against its
own hash at the far end — so publishing is a claim about something the server
already has, which is why the upload goes first and the claim second. A publish
naming a track the server has never seen is refused rather than recorded as an
empty promise.

The server will now serve any track it holds rather than only the one its lobby
is racing, which is what makes a published track *playable* rather than merely
listed. A listing alone would have passed a weaker test.

### The generator

`src/core/gs_generate.{h,c}` turns a number into a track. Four shapes — a
sprint, a circuit, a run of jumps, and mixed surfaces — chosen from the seed,
with painted gravity over the middle two thirds of the time and a two-word name
from the same seed, because "seed 2864434397" is not a thing anybody says out
loud. `gearstick_cli generate [N]` sweeps N seeds and races every track through
the analyser: `generate 50` is the item's verification and `generate 200` also
passes.

**Three properties are built in rather than tuned in**, each of which the sweep
caught when it was not:

- *No slope steeper than a car can climb.* `gs_lay_ridge` derives its ramp from
  the height it was given rather than taking both as arguments, so a ridge is
  driveable by construction. `GS_MAX_CLIMB` moved from `gs_sim.c` into
  `gs_sim.h` for this: the generator is bound by the simulation's limit, and a
  limit written down twice is a limit that will disagree with itself. Ridges add
  to what is underneath rather than max with it, so a ridge on a bowl is a ridge
  on a bowl instead of a step where the two meet.
- *Nothing is built in the first fourteen tiles.* A car starts still. With six
  tiles of run-up a stock car reaches two tiles a second and cannot crest a
  two-tile ridge, and the track then comes back undriveable for a reason that
  has nothing to do with its shape. Perturbing `GS_GEN_RUNUP` down to three puts
  two of fifty tracks beyond anybody.
- *A generated height is never zero.* A ridge of no height is not a shallower
  ridge, it is a missing one; two of the first fifty seeds came out as flat
  fields before `gs_height` was floored at one quarter-tile.

**The generator's output is now a golden number**, `GS_SELFTEST_GENERATOR_HASH`
in `src/frontend/cli/golden.h`, folded over its first two hundred seeds and
checked by `gearstick_cli selftest --verify` — which every CI platform already
runs. It was added because it immediately caught the reason it needs to exist:
**gcc and clang generated different tracks from the same seed.** Two calls to
the RNG sitting in one argument list are two calls in an order C does not
define, gcc took them right to left and clang left to right, and a seed
therefore named different ground depending on who built the binary. Every draw
now gets its own statement. A generated track is identified by its seed, so this
is the same class of break as a physics desync.

### Three things the analyser was getting wrong

The generator found them, because it was the first thing to ask the analyser
about tracks nobody had already checked by driving.

**Cars started on the start line.** `gs_analyse` placed its car exactly at gate
zero, which left it with its own position to aim at and no reason to go
anywhere; it wandered until it happened to cross its own line backwards-first
and then drove the lap. Whether it recovered depended on how much room it had to
wander, so perfectly driveable tracks came back impossible. `gs_track_grid` in
`gs_track.c` now says where a car waits for the flag — behind the line, abreast
across it, facing the way the route runs — and the analyser and the game's race
start both use it. The game had been placing cars at a hard-coded `x = 3`, which
was right for the stock tracks and wrong for everything else.

**The heatmap was drowned by cars that had already finished.** The run went on
for the whole time allowed, so a car that completed its lap kept driving and the
corner it happened to mill about in collected more visits than the entire racing
line — leaving the line under a fifth of full heat and invisible. Runs now stop
at the flag, which is also what makes the heat mean "the line everybody drove"
rather than "where everybody ended up".

**The time allowed was a constant, and every caller chose a different one.**
Twenty seconds is generous on a forty-tile sprint and runs out halfway along a
fifty-two-tile out-and-back; the verdict then described the clock rather than
the track. `gs_analyse_seconds` derives it from the length of the route at a
pace slow enough that failing to keep it means the track. The editor, the sweep
and the tests all ask for it now, so they cannot disagree about whether a track
can be got round.

### The game had no sound

Found while asking why a captured `.wav` could not be made to race against the
sound card. It could not, because there was nothing to race: `SDL_Init` was
given video and gamepads and **not audio**, so every `SDL_OpenAudioDeviceStream`
failed with "Audio subsystem is not initialized", `gs_audio_open` took its
no-device path — which is deliberate and correct, because a machine with no
sound card should still race — and the game ran in silence on every machine
there has ever been. The synthesiser, the tyre filters and the seeded music were
all fine and all tested; none of it was ever connected to a speaker.

The audio tests had been passing about five times in six. They opened SDL's
dummy driver, which is still a driver and still runs a callback thread; that
thread mixed the music while the test measured it, so *the same seed is the same
music* was true or false depending on when the callback fired. A test that only
sometimes tests its rule is worse than none, because the green tick is not
evidence. `gs_audio_open_silent` brings the mixer up with nothing behind it, and
the tests now use it: twenty-four consecutive runs, no failures, against six
failures in twenty-four with a device thread running.

`--audio-out` uses the same silent path and no longer asks for the audio
subsystem at all, so what lands in the file is what would have come out of the
speaker rather than half of each. Captures are byte-identical run to run.

CI was setting `SDL_AUDIODRIVER`, which is SDL2's name for the variable; SDL3
reads `SDL_AUDIO_DRIVER`. It had never mattered, because the subsystem was never
up. It does now.

### The tracks that ship, and the library that carries them

Sixteen tracks in `assets/tracks/`. Four are still written out by hand in
`tools/make_tracks.c`, because a first track wants a shape somebody chose. The
other twelve come out of the generator and are **kept by being driven**: every
one of the six vehicles has to get round at Earth gravity, or the seed is passed
over. Earth specifically rather than the analyser's whole range — the analyser
answers "can this be got round at all", which is the right question for somebody
looking at their own work and the wrong one for a set that ships to a player who
picked a car and left the dial where it was. Three per shape, so the set is a
menu rather than twelve variations on one idea.

Of a hundred and twenty generated tracks, ninety clear that bar, so the
constraint is real without being scarce.

`assets/server/gearstick.db` is the library a server ships with: every stock
track, published, built by `tools/make_store.c` through the same `gs_store` the
server uses. A server started with no store at all copies it into place and
comes up with sixteen tracks to offer, rather than an empty list and no reason
for the first visitor to stay.

**Committing a binary needs two guards, and it has both.** A test opens the
shipped file and exercises every table the server writes to, so a schema change
that forgets to rebuild it turns the tree red instead of failing in somebody's
hands; and a CI job rebuilds the tracks and the database and diffs the result,
twice, so the committed bytes are the built bytes and the choosing is
reproducible.

That second guard needed one thing fixing before it could work, and the mistake
is worth recording: **the database was not reproducible, and the check that said
it was, was wrong.** `gs_store_put_track` stamps a track with `strftime('%s')`,
so the file differed on every build — three builds seconds apart shared a
timestamp and looked identical, which is what the first measurement caught and
why it was believed. A rebuild an hour later differed in thirty-two bytes, all
of them clock. The tool that builds the shipped library now dates its tracks at
the epoch, because a stock track's "added" date means nothing — it shipped with
the game — and a file in a repository has to be the same file every time it is
built. A test reads the dates back out of the shipped copy and requires them to
be zero.

The server now takes what it serves **out of the store**. `--track FILE` still
works and now means *import*: the file goes into the library, is published, and
is then served from there like anything else. With no `--track` it serves the
first published track it holds. A file named on the command line was the last
thing the server knew that its database did not.

Two smaller things came out of it. Seeding a store by copying bytes into place
is unsafe if SQLite's write-ahead log from a previous database is still lying
beside it — the next open replays it, and yesterday's rows appear inside today's
library. A journal whose database does not exist is orphaned by definition, so
seeding removes it. And the `*.db-wal` and `*.db-shm` files that tests had been
leaving in the repository root turned out to be *committed*; they are gone and
`.gitignore` now covers them, with an exception for the one database that really
does ship.

### The HUD, and a camera that was twice too far out

Lap, position, the lap being driven, the best so far and what is left of the car
— on screen while it is still worth knowing, rather than on the results table
afterwards. `src/ui/gs_hud.c`, one panel per view, so split screen gets one per
driver clipped to that driver's quarter of the window.

Position is `gs_world_place` in the simulation rather than in the HUD that shows
it, because it is a fact about the race and not about the drawing: laps first,
then which gate you are heading for, then how far along that leg you are. The
last part is what makes the number change when the racing changes rather than
twice a lap when somebody crosses a line. A finished car keeps the place it
finished in.

The HUD is sized here rather than auto-fitted. An auto-resizing ImGui window is
invisible for its first frame and a screenshot is one frame, so an auto-fitted
HUD would be on screen for a player and absent from every capture — a bug
nobody would notice and a verification that could not work. The same trap cost
an afternoon on the editor's palette once already.

**And the camera was twice as far out as its own comment claimed.** `gs_iso.h`
said a split-screen pane showed ten tiles; it showed twenty. At the old default
a full window held about nine hundred tiles of ground and a car was three and a
half percent of the screen's width, against roughly seven and a half on a C64
with a fifteenth of the pixels. The ratio argument in that comment was right and
had been acted on — the meshes are 1.3 tiles — but the distance half never was.
At 2.0 a tile edge is seventy-two pixels, a car is ninety-four, and a pane shows
the ten tiles the comment always meant.

Both HUD tests compare frames with the ground and the cars in *identical*
positions, which took two attempts: the first version moved the car between
captures, so the pixels in the HUD's corner differed whatever the HUD did, and
pinning the position to a constant did not fail it. Perturbing all three of the
numbers it draws now fails the test that names them.

### Nine grounds, and how they were kept apart

The gravity dial has always named eight worlds, and there were three surfaces,
all of which could have been a car park. There are now nine: sand, gravel, rock,
dust, slush and grass beside pavement, dirt and ice. The enum is **appended to
and never renumbered** — the value is what a saved track stores, so moving one
would silently change the ground under every track anybody has built. The world
hash confirms it: it did not move.

**The rule each one had to pass is that it is a different thing to drive on.**
That is measured rather than argued, on four counts: flat-out speed, how long to
reach three tiles a second, how fast a full-lock circle settles, and how much
that circle changes once the tiles under it are ground flat. Every pair of the
nine differs by at least a sixth on at least one.

Getting there took two corrections worth recording.

*Lap time alone does not separate them.* The first measurement was one car round
one track, and it put gravel and grass 0.07 seconds apart and dust and slush 0.06
apart. Two grounds can reach the same lap time by being bad at different things —
one robs you of drive, the other of grip — so a single number folds a real
difference into a coincidence.

*Neither does a fresh surface.* Adding wear to the measurement is what finally
separated dirt from gravel, which sit close on every fresh-surface figure and
behave differently the moment anybody has driven on them. Wear is one of the
three things a surface definition controls and ignoring it under-counted the
distinctions the design had already made.

The palette was measured too, and failed first time. Gravel, dust and rock came
out as the same grey at three brightnesses — which reads perfectly well on a flat
plane and disappears the moment the ground tilts, because shading changes a
tile's brightness by more than those differed by. Two surfaces separated only by
how pale they are are one surface on a hillside. Every pair is now more than 0.15
apart in RGB, and a test says so.

Two smaller things fell out. The editor kept its own list of three surface names
against nine surfaces — a combo box reading past the end of its own array — and
now takes them from the surface table, because a name written down twice is a
name that will disagree with itself. And the generator picked its ground from a
hard-coded three; it picks from all nine, which moved the generator's golden hash
deliberately and with a note.

### Wreckage, which was mostly already there

Worth recording honestly: this item was largely finished by the car-to-car
collision work in Phase 7 and nobody had noticed. A wrecked car already stopped
being simulated, already refused to be pushed, and already bent the line of
anything that hit it, with three tests pinning exactly that. Checking before
building is what found it.

What was actually missing is that **debris is bigger than the car it used to
be**. A wreck occupying precisely the footprint of the car that made it is a
parked car, and "winning the fight reshapes the course" would be a sentence about
a parking space. `GS_WRECK_RADIUS` is half again `GS_CAR_RADIUS`, the collision
takes each car's own size rather than assuming both are alive, and the renderer
spreads and flattens the mesh and its shadow to match — so what a player sees in
the way is the size of the thing that is in the way. A wreck drawn car-sized over
a wreck-sized obstacle would be the worst of both: it catches you on something
you were shown you would clear.

Both halves are pinned. A car passing at an offset that clears a live car does
not clear a wreck; and on the drawn frame, the debris covers about seventy per
cent more ground than the car did. That second measurement took two attempts —
the first compared each pixel against the flat pavement colour from the palette,
which marks the entire frame as interesting, because the terrain is shaded and a
pavement pixel is nowhere near the unshaded pavement colour. The reference now
comes from the picture rather than from the palette.

### The landing arc

**J** while airborne draws where you are going to come down: a dotted line that
fades along its length, and a ring on the ground at the touchdown. Off by
default, and the guide says to leave it off once you have the measure of a ramp —
knowing where you will land is not the same skill as judging it, and the judging
is the better game. It earns its place in the construction set, where "can
anybody clear this" is the whole question.

**It is not a parabola.** `gs_world_arc` copies the world, takes the other cars
out of the copy, and steps it forward with the real `gs_world_step` until the car
is grounded. An airborne car has drag on it and gravity is sampled per tile, so a
closed form would be an approximation of this program rather than a description
of it — and an arc that disagreed with the landing would be worse than no arc,
because it is believed. Being the same code is why the test can demand the landing
match *exactly*, to the fixed-point value, at three gravities.

Other cars come out of the copy because a mid-air collision is not predictable.
What the arc says is where you land if nothing hits you, which is the question
worth answering.

The first version had a bad contract and the test caught it. Given one point per
tick and a fixed array, a flight longer than the array simply stopped — and a
path that stops still ends *somewhere*, which is read as the landing. It now
halves its own sampling rate in place when it runs out of room, so the last point
is the touchdown however long the car is up there, and a `landed` flag says
whether the flight ended or the car has left the world entirely.

### What surrounds a track

**A run-off, and then a drop.** Ten tiles of sand outside the authored tiles, at
the height of the edge they left, and past that the ground falls away at three to
one — steeper than `GS_MAX_CLIMB`, so there is no driving back up it. A car more
than three tiles past the shoulder is finished.

The complaint that started it was that leaving cost nothing. The deeper problem
was that **what a player could see and what they could drive on disagreed**:
nothing outside the track was drawn, so the track ended in blackness, while the
physics clamped to the edge tile and handed them an infinite invisible plain. The
surround is drawn now, darkened so the racing surface is still plainly the bright
part.

Four things this taught, all of them by breaking:

**A run-off works by drag, not by slipperiness.** The first version used dust, on
the reasoning that it is loose. It is, and it also has almost no rolling
resistance, so a car that ran wide kept every bit of its speed and sailed across
to the drop. Sand is the draggiest of the nine grounds; a car that brakes on
reaching it stops inside it. Switching that one constant fixed more failing tests
than any amount of tuning had.

**A car does not fall off the edge, it drives down it.** The rule that ended a
departure lived in the airborne branch and waited for the car to be a few tiles
below the ground — which never happened, because following the ground keeps up
with any gradient going downhill. Cars sledged twenty tiles down the face over
seven seconds and were eventually wrecked by arriving at the bottom. It is
measured as distance out now, and a departure ends where it happened.

**An unbounded drop is not a number.** Falling three tiles per tile forever
overflowed Q16.16 for a car thrown a few thousand tiles off at low gravity, and
the ground came back *above* the car. Found by the roster sweep under UBSan. The
drop bottoms out at sixty-four tiles, far below anything recoverable.

**The AI had been driving off the world all along.** It never mattered while the
plain was infinite. Teaching it took three attempts and the lesson is the third:
an edge-avoidance term that nudges the aim point away from the boundary fights
the route it is meant to be following — too wide and the car drives to the middle
of a narrow track for ever, too narrow and it never turns in time. What actually
works is nothing to do with steering: brake for the edge the way the corner
planner already brakes for a gate (using the run-off's grip, not the road's,
because that is where the braking happens), aim back onto the track once off it,
and let the sand do the rest. Removing the steering bias fixed both the sweep and
two generated tracks it had broken.

One separate AI gap came out with it: a car that had gone past a gate aimed at
its centre from the wrong side, drove at it, arrived from the front and could not
cross — then circled beside it for ever. It aims at a point four tiles behind the
gate now, so an overshoot becomes a deliberate loop. That was always broken; an
infinite plain let the car wander until it blundered back through.

The golden replay moved, deliberately: the selftest race has a car that leaves
the track.

### The route joins the undo history

Undo covered the terrain, the surfaces and the gravity, and not the gates. That
made "undo covers what you can change" a promise with a footnote, and the
footnote was the part that decides what a track *is*: every record, every ghost
and every shared code is keyed on the hash, and the route is in it.

`gs_edit` carries a `gs_gate` now, which costs every entry sixteen bytes it does
not use — a log twice the size, against an undo history with no gap in it. The
second is worth more. Adding and removing are recorded as each other's reverse,
so undo and redo need to know nothing else, and a removal remembers its *place in
the order* as well as its numbers: a route is a list, and a gate put back on the
end is a different track from one put back in the middle. The test checks the
hash rather than the count for exactly that reason.

### And the committed database needed its writer pinned

CI caught what local testing could not. The drift job rebuilt the shipped library
and diffed it, and it differed — not by content, which was identical row for row,
but because the runner had **SQLite 3.45 from the system** and this machine has a
different version. Two versions store the same rows in a different arrangement of
pages, which no query can see and a byte-diff cannot miss.

So the writer of a committed artefact is pinned, the same way the committed trig
table is pinned to the script that bakes it: `gearstick_make_store` is built
against the amalgamation, and the job that checks it configures with
`-DGEARSTICK_SQLITE_AMALGAMATION=ON`. With the writer fixed the file is
reproducible; with it floating, "reproducible" was a property of one machine.

### The store survives a format change

Profiles and records refused anything but their own version, which reads like
caution and means somebody's history disappears the first time a field is added
— and a field always is. Both are at version two now, and both readers still
accept version one.

**The format had to actually move, or the migration would be code nobody has
ever run.** So each gained the thing it was missing: a record says when it was
set, a profile when it last drove. Both are Unix times passed in from outside,
because `src/core/` links nothing and a simulation that could read a clock is a
simulation whose answer depends on when it ran. Version one rows load with zero
there, meaning "not recorded" rather than the epoch, which is the truth.

The test writes the old layout **byte by byte with a frozen writer kept in the
test file**. Generating it with the current code would prove nothing at all: the
two would move together, and the day somebody changes the layout the "old" file
would change with it.

Tolerant of the past and not of the future: a version this build has never heard
of is refused, because a table of times read by guesswork is worse than one that
will not open.

One thing the perturbation caught that a passing suite never would: the test
dereferenced the record it looked up, so when the loader was broken on purpose
the suite died with a segmentation fault instead of naming the fact that had
stopped being true. A test that crashes reports nothing.

### A track from somebody else's game

Every track this project has seen was built by whoever wrote the editor. That is
the worst possible sample — the shapes that get built are the shapes the tools
make easy — and a generated track does not help, because the generator was
written by the same person from the same assumptions.

`src/core/gs_stunts.c` reads a Stunts (1990) `.trk`. The format is published and
small: 1802 bytes, a 30×30 grid, 900 bytes of road pieces, one byte of horizon,
900 bytes of terrain, and a trailing byte nobody has explained. Stunts has three
road surfaces — paved, dirt, ice — which are the three this project started with,
so they cross exactly. Its two elevations become heights, and the slopes between
them fall out of our bilinear sampling rather than being built as well, which
would give them twice the climb.

Two things about the layout are easy to get wrong and both are caught by tests:
the two 900-byte planes are indistinguishable by inspection, and the road plane
is stored bottom to top while the terrain plane is stored top to bottom. Swapping
either produces something that still looks like a track.

**Anything this reader cannot name becomes road, and is counted.** Stunts has
loops, pipes, corkscrews and bridges, and the element table this was written from
does not list them all. Laying road says the true thing — a car should be able to
drive where the donor put one — and the count says the other true thing, which is
that the shape was lost. Leaving them as grass would silently cut a track in half
and report success.

**What is verified, and what is not.** `gearstick_cli import` runs in CI against
a file written in the donor's layout by this repository, converts it, validates
the route and has the analyser drive it. That proves the reader and our writer
agree with each other. It does **not** prove the reader agrees with a real Stunts
file — nothing here has ever seen one. Closing that gap needs a person to point
it at a downloaded track, because the corpus is somebody else's and does not ship:
`docs/ASSETS.md` rule 1 makes no exception for tracks, and a corpus is no
different from a sprite sheet.

### The stock tracks, and what the 1985 manual was actually worth

Its section 7.0 lists all fifty of the original's tracks with a line on each.
The list is theirs; **the categories are the part worth having**:

- shapes named for their shape — a figure of eight, a clover, a spiral;
- challenges named for what happens on them — `jumps` ("big ones"), `headon`,
  which "aims drivers directly at each other", `whichway`, offering "seven
  different routes";
- test courses after real ones — Fiorano, Weissach, an oval;
- and thirty-odd real circuits, all pavement, no jumps, Earth gravity, five laps,
  which is itself a statement about what a plain track is for.

The lesson is not which tracks to build. It is that **every one of them had a
reason somebody could say in a line**, and a set that cannot do that is a set of
variations. Six new designed tracks answer to those categories — the crossing,
head on, which way, the oval, the big one, the long way round — joining the four
that were already there, and `docs/GUIDE.md` prints each one's reason where a
player will see it.

And `gearstick_make_tracks` now races every designed track through the analyser
before writing it. The generated twelve had always been chosen by driving them;
the hand-built ones were only route-checked, which quietly made the designed half
the *less* verified one. It costs a few seconds at build time. It also caught the
first draft of the oval immediately, though not for that reason: a gate's line
runs across its heading, so a wide gate near an edge hangs off the track, and the
validator said so.

One thing the perturbation turned up that is worth writing down: a wall built
clean across a corridor no longer makes a track uncompletable, because a car can
leave the track and come back round it through the run-off. That is the surround
working as designed — going round is slow, not impossible — but it means "put a
wall across it" is no longer a way to construct an impassable track.

### A recording says who drove it

`docs/THREATS.md` called this the most serious open hole and it was: a replay
carried the track, the dials, the grid and the machines and not the driver, so
an honest recording was a bearer token. Anybody who obtained one could hand it
in and the verifier would correctly agree the time had been driven — it had,
just not by them.

`gs_replay_meta` now carries a driver per car. That is replay version 4; version
3 is still read, with its names blank. The claim carries who is submitting, and
`gs_verify` refuses a mismatch with `GS_VERDICT_WRONG_DRIVER`. A recording that
names nobody backs nobody's claim, because "it does not say" is not "it says
you" — and a caller asserting no identity at all still gets an answer about the
driving, which is what a local ghost or an offline analysis wants.

### A submission is bound to the session that asked for it

Records are keyed on track, conditions and lap count, so handing the same replay
in twice already set one record rather than two. That was **idempotent by
accident of the schema, not a defence** — and a thing that is safe by accident
stops being safe the next time the schema changes.

The server now issues a one-shot nonce when it places a client and again after
every claim it resolves. `GS_MSG_SESSION` carries it out, the claim carries it
back, and `gs_store_spend_session` retires it. Four things have to hold: the
nonce was issued, it was issued to *this* driver, it is unspent, and it is in
date.

Three decisions in that are worth the words:

- **All four conditions live in one `UPDATE`, and the change count is the
  answer.** Reading the row and then writing it would be two steps with a gap
  between them, and the gap is exactly where the same nonce gets spent twice.
- **The nonce is spent after the re-race, not before.** Re-racing is what says
  the time is real; a nonce burnt on a claim that turned out to be nonsense
  would cost an honest client its next submission for somebody else's mistake.
  A rejected claim gets a fresh nonce rather than a dead end.
- **Sessions are rows in the database, like everything else the server knows.**
  A server holding them in memory would forget every one on restart, and a nonce
  nobody can retire is a nonce that can be handed in for ever — which is the
  whole thing it exists to stop.

The expiry check is the part that had to be watched fail. It was written into
the comment before it was written into the SQL, where the fourth condition sat
as a placeholder that was true for every row; the store test that asks for a
token to be refused an hour after it was issued is what said so.

**Six tests, and every one of them seen red.** Two in `test_store.c` pin the
mechanism — a token is good once, only to whoever it was issued to, and only in
date; and a token spent by one process is still spent when another opens the
same file. Four in `test_server.c` pin it through an actual server: a claim
carrying a nonce the server never issued is refused, one carrying a nonce issued
to a different driver is refused, a second submission on the same nonce is
refused, and a nonce spent before a restart is still spent after it. Removing
the check turns all six red, which is how they are known to be testing it.

The three server tests each carry a **positive control**: the same driver, the
same recording, the same time, resubmitted with a nonce that *is* good, and it
lands. Without that, a refusal proves only that something went wrong, and the
test would pass just as happily if the claim were being rejected for a reason
nobody intended. The cross-driver test needs a recording that is honestly bez's
for the same reason — a bez claim backed by ada's recording is refused for
naming the wrong driver and never reaches the session check at all.

**Two limits, stated in `THREATS.md` rather than left to be discovered.** The
nonce comes from `SDL_rand_bits`, which is not a cryptographic generator: this
is the shape the defence will take and not yet a defence against somebody who
can predict it. And it travels in clear over the same unauthenticated channel as
everything else, so anybody on the path can read one — what stops them spending
it is the name it was issued to, and a name is worth something only once the
transport and the accounts land.

### A race commits to its inputs before it sees anybody else's

The state-hash check between peers catches a modified *simulation* and is
completely blind to a modified *decision*. Rollback hands every peer the others'
inputs for a tick, so a client that waits, looks, and then chooses desyncs
nothing at all — everybody simulates the dishonest input faithfully, and every
machine agrees about a race that was rigged.

So an input is promised before it is shown. Each datagram carries a commitment
for the most recent twelve ticks and the input and salt themselves for the
thirty-two ticks before those. A reveal that does not match its promise stops
the race — not stalls it: a stall ends when a datagram arrives, and there is no
datagram that makes this all right.

**The rule the whole thing rests on is that a promise only counts when it
arrives in a datagram that does not also prove it.** Let the two travel together
and the promise costs nothing to make: a peer could wait, choose, and build both
at once, indistinguishable from an honest one. That is the entire reason the
reveals run a fixed distance behind the commitments. A peer that ignores the gap
is not accused of anything, because it has not lied — it simply never says
anything checkable, and nothing it sends is accepted.

**The cost is twelve ticks, and it is paid by the remote car only.** The local
car still responds to its own pad on the frame the key goes down; what arrives a
tenth of a second later is the *correction* for somebody else's car, which is
what rollback exists to absorb. Sending commitments for more than twelve ticks
would be waste, because a thirteenth copy arrives in a datagram that also
reveals the tick and is inadmissible by the rule above.

#### BLAKE2s, and why there is a hash in the simulation

`gs_world_hash` is FNV-1a. That is right for noticing two machines have stopped
agreeing and useless for a promise, which has to be one the promiser cannot
wriggle out of. So `src/core/gs_blake2s.c` is RFC 7693, integer-only, no
allocation — core links nothing and cannot reach libsodium, and will not be able
to when libsodium arrives for the transport.

It is checked against the RFC's published vector for `"abc"` and against
Python's `hashlib` for the lengths the RFC does not cover. Those lengths are
chosen rather than arbitrary: 63, 64, 65 and 128 bytes bracket the block
boundary, which is where a buffered hash goes wrong if it compresses a full
block eagerly instead of waiting to learn whether anything follows it. It is
also fed a message one byte at a time and has to agree with itself.

#### Two faults this turned up

**A rollback datagram outgrew the link and nothing said so.** The packet went
from 54 bytes to 411. `gs_net_packet` refuses to write into a buffer too small
for it and returns zero; the simulated link in the tests silently dropped
anything over its own 96-byte limit. Between them, every race stalled with no
input ever crossing — which reads exactly like a network fault and was a
constant nobody had revisited. The size is now a constant in `gs_net.h`,
`gs_wire.h` carries a `static_assert` that a wire datagram is at least that big,
the test link uses the real figure, and it counts oversize packets separately
from the loss it inflicts on purpose so the two can never again be confused.

**A stalled peer rewrote a promise it had already made, and was thrown out of an
honest race for it.** When the window fills, `gs_net_step` stops advancing but
the frame loop keeps running, and a caller polling the pad every frame handed
over a different input for the tick it was stuck on. That input had already gone
out inside a commitment. The far end saw one tick promised two different ways —
which is precisely what a peer choosing late looks like — and correctly stopped
the race. `gs_net_local_input` now keeps the first word said on a tick and
ignores the rest. The test for the admissibility rule is what found it.

#### What is pinned

Four tests, each watched failing with its own rule removed and nothing else:
the published and independent digest vectors; a peer whose reveal does not match
its promise being caught and the race stopping; a peer promising two different
things for one tick being caught; and a peer that shows its inputs in the same
breath as it promises them getting nowhere at all. The four-player race over a
bad link still agrees tick for tick with the race one machine would have run
alone, which is the claim that says the commitment costs correctness nothing.

### The whole race is verified, not just the winning lap

Every check the verifier had was about one car and one lap: this track, these
rules, this driver, a claimed time no better than the driven one. A log altered
anywhere outside the claimed lap walked past all of them.

A networked recording now carries `agreed_hash` — the state every peer agreed
the race ended in — and re-racing the log has to arrive there. That is a
statement about every tick and every car at once, and it costs one comparison,
because the simulation is exactly reproducible. This is the argument for having
built it that way, collected.

Replay version 5. Versions 4 and 3 still read, with the agreed ending zero,
which means **"this recording does not say"** and not "this recording agrees
with anything" — so a race run on one machine, where there is nobody to have
agreed with, is still checked for the lap it claims and is not failed for the
absence.

#### An online race was recording nothing at all

Found on the way, and it is bigger than the item that found it. The networked
branch of the frame loop stepped the rollback session and never called
`gs_replay_record`. The submission path then serialised whatever the last
offline race had left in the buffer. **A networked time could not have been
verified by anybody** — the item that made the server re-race every submission
was, for online races, verifying a recording of something else.

Fixing it is not a matter of adding the missing call, because there are two
worlds and only one of them is the race:

- **The visible world is built partly on guesses** about the other cars, and
  most of those are rolled back. A recording taken from it would be a recording
  of things that did not happen.
- **The confirmed world is built only from inputs every peer actually sent.**
  That is the race, and `gs_net_confirmed_input` hands it over a tick at a time
  as it is agreed.

#### And the race ends twice

The visible race crosses the line about a dozen ticks before the agreed one
does, because the reveals trail the commitments. Submitting at the moment the
player sees the flag would hand in a recording that stops short of its own
ending and cannot reproduce it.

So the client keeps talking after the finish. `gs_net_settle` flushes what is
still owed, keeps receiving, writes down each tick as it is agreed, and submits
only once the *confirmed* world is over — stamping the agreed ending into the
recording as it goes. If nobody finishes agreeing within about fifteen seconds
it submits nothing and says so, which is better than submitting half a race.

#### What is pinned

Four peers race six seconds over links with different latencies, jitter and one
datagram in eleven dropped. All four write down the same log, of the same
length, ending in the same state, and all four logs are accepted.

Then one bit is flipped in the middle of the log, in a car other than the one
being claimed, and it is refused — **and the same flipped bit, with the agreed
ending taken off the recording, is accepted.** That second line is the test: it
measures the hole rather than asserting the patch.

The claim used carries no times at all, deliberately. It means every check that
existed before this one passes whatever the log says, so the only thing that can
catch a doctored log is the ending it has to produce. A claim with times in it
would be caught by the lap check too, and the test would no longer be testing
this rule.

The sweep across ticks and cars states the rule exactly: a log is refused when
it re-races to somewhere other than the agreed ending, **not** merely when its
bytes have changed. Those are different claims. By five hundred ticks in some
cars are wrecked, and a wrecked car is not taking input, so flipping its
accelerator alters the log and alters nothing that happened — answering
"verified" there is right. The test asserts the correspondence in both
directions and then asserts that most of the flips did move the ending, so it
has teeth rather than agreeing with itself about nothing.

**Not covered by an automated test:** `gs_net_settle` itself, which needs a real
frame loop and a real socket. What the test drives is the same sequence the
frontend drives — record each confirmed tick, stamp the agreed hash, verify —
through the same functions, but the frontend's own wiring of it is checked by
reading rather than by running.

### The parsers, fed rubbish

Every byte the server acts on came from somebody who may be hostile, and until
this landed those parsers had never been fed anything but well-formed input.
There are four libFuzzer targets — the protocol decoder, the chunked
reassembler, the track deserialiser and the replay deserialiser — built with
clang under ASan and UBSan, off by default and run in CI.

Two of them go past parsing on purpose. A track that deserialises is then
hashed, sampled at every corner and driven off the edge of, because a parser
that lets an out-of-range dimension through does its damage in the code that
trusts the result. A replay that deserialises is re-raced, because every field
the parser accepted — the car count, the vehicle ids, the grid, the tick count —
is an index or a loop bound in the playback.

**The corpus is generated, not committed.** `gearstick_fuzz_seeds` builds real
messages with the same code the game builds them with, so a seed cannot quietly
stop being a sample of the format it is supposed to be a sample of. It writes a
dictionary from the same constants, because each of these formats opens with a
four-byte magic word and mutation reaches four specific bytes by luck alone.

Two things run: a fixed amount of work over the seed corpus as an ordinary
`ctest` test, which is the regression half and must never go red; and a timed
campaign per parser in CI, which is the half looking for something new.

#### What it found, and what it did not

**No crashes.** Around 120 million executions across the four parsers under ASan
and UBSan found nothing. That is the honest result and it is worth stating
plainly rather than dressing up: these parsers were written carefully and the
fuzzers agree so far.

**The carrier harness was nearly useless, and planting a bug is what showed it.**
An out-of-bounds write was introduced past the reassembler's length check and
four and a half million runs sailed by without noticing. The reason was in the
harness: it took the expected track hash from the front of the fuzzer's input,
and every chunk carries the hash of the track it belongs to, so any mutation
broke an eight-byte equality and the input was refused at that one comparison
before reaching any reassembly at all. It now takes the hash from the first
datagram that parses — which is also what the server does, from the claim — and
a later chunk disagreeing is the stray-chunk case, still reached and now reached
on purpose.

**The track target does find planted bugs.** With the bound on `w` and `h`
removed from `gs_track_deserialize`, ASan reported a SEGV with the file and line
within seconds.

#### The reassembler's own bound

The item asked for the reassembler to bound its own array index rather than
inherit the bound from a check in another file, and it now does. **Said plainly:
this was not a live hole.** `gs_proto_read_track_chunk` refuses a datagram whose
chunk number is not below its own count, and `gs_carrier_take` refuses a count
larger than the array, so the index was already in range.

The point is that this was an argument rather than a bound. It ran through two
checks in another file and the arithmetic relating `GS_CARRIER_MAX_BYTES`,
`GS_CHUNK_BYTES` and the rounding between them — and part of it held only
because `GS_CARRIER_MAX_BYTES` happens not to be a multiple of `GS_CHUNK_BYTES`.
Round the replay length one day and a chunk index of exactly
`GS_CARRIER_MAX_CHUNKS` starts passing the byte check and writing one past the
end of `have`. Nothing in either file said so.

#### One live defect, found while writing the test

**A refused chunk left its number behind.** The declared chunk count was written
into the transfer before the remaining checks had run, so a datagram that was
about to be rejected still poisoned the count — and every honest chunk that
followed was then turned away for disagreeing with a number that came from the
chunk nobody accepted. One malformed datagram and that track could never be
received again. Everything is now checked before anything is kept.

It also caught the first draft of its own test, which reused one track hash for
every case. `gs_carrier_expect` with the hash it is already collecting is a
no-op by design, so state carried between cases and the refusal came from the
wrong rule — the test passed with the bound removed. Each case now uses a
different hash.

### The tunnel — Noise_IK_25519_ChaChaPoly_BLAKE2s

**Not finished: nothing uses it yet.** The tunnel exists, is tested, and
interoperates; the game still sends everything in the clear. The wire
integration is the remaining half of this item and the plan entry stays
unticked until a race runs through it.

What is built is `src/net/gs_noise.c`: the IK handshake pattern from the Noise
Protocol Framework, revision 34, spelled as the specification spells it, over
X25519 and ChaCha20-Poly1305 from libsodium and BLAKE2s from `src/core/`.

**IK rather than NK** because the client already knows the server's static
public key, so one round trip buys server authentication, client
authentication, and a client identity a passive observer cannot read. **One
suite and no negotiation**, because a protocol that cannot negotiate cannot be
talked down to something weaker. The suite name goes into the handshake hash
before anything else, so two endpoints that disagree about it cannot complete a
handshake at all.

#### A datagram channel, which Noise does not assume

The framework's transport phase assumes messages arrive in order. Nothing this
project sends does. So each datagram carries an explicit 64-bit counter and the
receiver keeps a 64-wide sliding window: anything already seen inside it, or too
far behind it to judge, is refused.

**Out of order is not the same thing as replayed**, and a rule of "must be newer
than the last one accepted" cannot tell them apart — it throws away the
reordering every lossy link produces constantly and calls it an attack, which in
a race means discarding inputs that will never be resent.

Two decisions worth the words:

- **The counter travels in front and is not passed as associated data.** It
  looks like it should be, and it does not need to be: the counter *is* the
  nonce, and the nonce is an input to the tag, so a counter changed in flight
  produces a tag that does not verify. Passing it as AD as well would add
  nothing and would make every sealed message differ from what the framework
  specifies — which would cost the published vectors and the interoperability
  check, because both cover the transport phase and not only the handshake.
- **The window moves only for a datagram that turned out to be genuine.**
  Recording the counter before the tag is checked would let anybody who can send
  a packet mark a sequence number as seen and have the real one refused when it
  arrived.

#### The evidence, which is deliberately not our own opinion

- **The framework's published test vectors pass.** From `cacophony.txt`, the
  Noise Protocol Framework's own published set, trimmed to this suite and
  committed as `tests/vectors/`. Every byte of both handshake messages, the
  handshake hash both ends arrive at, and all four transport messages after it.
- **A handshake completes against an implementation nobody here wrote.**
  `tools/noise_interop.py` drives `gearstick_noise_peer` from the Python
  `noiseprotocol` package, in both directions, and checks that both ends arrive
  at the same handshake hash. It is a separate program in a separate language
  because an independent implementation has to be independent. It reports itself
  *skipped* rather than passed when the package is absent, and CI installs it —
  a skipped check for the one piece of evidence that is not our own opinion is
  worth nothing.
- **A capture carries none of the plaintext it was given** — not the whole
  string and not any eight bytes of it, the second being the check that would
  catch a cipher that had quietly become a no-op for part of a message.
- **Every single bit flipped, one at a time**, in a sealed datagram, is refused
  — including the counter in front of it.
- **A replay is refused and a reordering is not**, tested by jumping forward and
  then delivering the skipped datagrams, each accepted exactly once, with one
  from far enough back that the window cannot say and is refused for that.
- **The handshake and the framing are fuzzed** under ASan and UBSan, seeded with
  real handshakes and real sealed streams. This is the most exposed parser in the
  program: a server has to look at a stranger's first datagram in order to find
  out that it is a stranger.

#### Two hours lost to a test, not to the tunnel

The published vectors failed on the first run and the implementation was
correct. `gs_field`, the little scanner that reads the vector file, returned a
pointer to one static buffer — so reading a payload and then a ciphertext
overwrote the payload, every message was compared against itself, and a
byte-for-byte correct handshake looked broken. What settled it was driving the
same vector through the Python library, which agreed with the vector, and then
printing our own bytes, which also agreed with the vector.

The Python driver had its own version of the same mistake first: it was handed
`init_remote_static` — a public key — through the API that takes a private one,
so the library disagreed with the vector too and briefly made the vector look
wrong rather than the driver.

#### libsodium, and the rule it bends

`ext/README.md` says nothing there is modified in place, and libsodium ships no
CMake — upstream builds with autotools and MSBuild. So `cmake/Libsodium.cmake`
names its sources and compiles them here, and the submodule stays exactly what
upstream tagged. Every source is compiled rather than the handful actually used
being picked out: a subset that is wrong links cleanly and behaves subtly
differently, which is the worst failure available in a cryptographic library.

It is also the one place "prefer no dependency to a small one" is set aside on
purpose, because the alternative to a large audited dependency here is our own
elliptic curve arithmetic, and that trade is not close.

### The tunnel, now carrying the game

**Still not finished, and the gap is named first: the direct peer-to-peer mesh
is not sealed.** Everything between a client and the server is; a race whose
peers can reach each other directly still exchanges rollback datagrams in the
clear. The plan entry stays unticked until that is closed.

What now goes through the tunnel is everything else — joining, the lobby, track
transfers, publishing, results, proofs, records, the heartbeat, the goodbye, and
a *relayed* race in full.

#### Where the seams are

Two chokepoints on each side, which is why this did not turn into a rewrite:
`gs_to_server` and the receive loop on the client, `gs_send` and `gs_handle` on
the server. The existing dispatch became `gs_handle_plain`, and above it sits a
shell that opens the envelope. Nothing below that line changed: by the time a
message reaches it, it came from the address it claims, has not been altered,
and has not been seen before.

**There is no plaintext path.** A client whose tunnel is not up sends nothing at
all rather than sending in the clear, and drops anything unsealed that arrives.
A fallback to plaintext is a fallback anybody on the path can force by dropping
one datagram. Everything is retried anyway — the join, the track request, the
heartbeat — so a message skipped for want of a tunnel comes round again.

The handshake is one datagram each way. IK's responder can answer immediately,
so the server keeps no half-finished handshake state and there is nothing for
anybody to fill up.

**A handshake from an address that is already racing is refused.** Message one
is replayable — anybody who captured one can send it again and the responder
cannot tell, because telling would need a session and there is no session yet.
Accepting it would install a tunnel whose keys the real client does not have and
knock a racing player off with a recorded packet. A client that genuinely
restarted waits out the silence timeout first, which costs it seconds and costs
an attacker the trick.

**The server has an identity and keeps it**, in the store with everything else
it knows, minted once on first run. It prints its public key at startup because
a client cannot connect without it: `--server-key` on the game, `--key` on the
server for a test that must know it in advance. A client handed no key refuses
to connect rather than connecting to whoever answers.

#### A real flaw the tunnel's own test found

The four-player relayed race under loss failed the first time, and it was not
the tunnel. **The last ticks of every race had exactly one admissible promise
each.**

A promise only counts when it arrives in a datagram that does not also prove it.
Once a finished race starts flushing, every datagram reveals the ticks it
commits — so those commitment copies are inadmissible. The commitment for the
final tick therefore had one admissible copy in existence: the datagram sent on
that tick. Lose it and the far end can never accept the reveal, and the race is
never confirmed to its end. Every other part of this protocol survives loss by
repetition and this part did not.

With one datagram in twenty dropped on each of two hops, three machines out of
four failed to confirm the last tick. In the real game that is a race that
never gets submitted, on a link that is otherwise perfectly playable.

The flush now waits: `gs_net_finish` asks for forty-eight more ordinary
datagrams first, which commit the final ticks without revealing them, and only
then do the reveals go out. Forty-eight chances is the same redundancy
everything else here relies on.

#### What the relayed race test actually claims

Four peers, each with its own tunnel to the server, modelled as the relay
really works: a datagram is sealed to the server, opened there, and re-sealed to
each of the others. Two encryptions per hop, two sessions per peer. One datagram
in twenty dropped on each hop and the rest reordered.

All four confirm the whole race, agree with each other, and agree with the race
one machine with no network would have run — which is the statement that says
the tunnel changed nothing about the driving rather than merely that the four
of them are consistent. About five hundred datagrams are dropped along the way,
so none of it passes by having nothing to survive.

### The mesh is sealed too, and nothing on the wire is in the clear

A race between two machines that can see each other goes straight between them,
so the tunnel to the server protects none of it. That path is now sealed as
well, and with it the last thing this game sent in the open.

**Every peer link is its own Noise IK session.** Which end initiates is settled
by slot number — the higher one does — because it has to be settled without a
negotiation, and that rule works everywhere this is used: a joiner reaching the
host whose address it typed has the host's key and a slot above zero; two peers
meeting through a broker have both keys and the rule breaks the tie. The
responder needs no key in advance, which is the property IK has and the reason a
host never needs to know a joiner before it knocks.

**Knocking is a handshake now.** It used to be five bytes of "hello" that
anybody could send and anybody could forge. It is the first message of an IK
handshake, so the host learns who is knocking from something they had to prove
rather than from something they claimed — which is what makes the roster it then
publishes worth anything to everybody else.

**The broker says who everybody is.** The server's lobby and the host's roster
now carry each player's static key, taken from the handshake that player
completed. A key somebody hands you about themselves authenticates nothing; a
key the broker watched them prove is what two peers meeting for the first time
check each other against. Both the lobby and the roster are themselves sealed,
so the list cannot be rewritten in flight.

#### Four bugs, three of them older than this work

**A datagram nobody sealed was taken for race traffic.** Anybody who knew a
player's address and port could send a rollback datagram and have it handed
straight to the race as somebody's inputs. There was nothing in the format that
said who wrote it, because there was nothing to say it with. Now nothing
unsealed is accepted from anybody, and there is a test that injects a
well-formed forgery and watches it be ignored — with a control that the real
peer's traffic still gets through, so it cannot pass by the client having gone
deaf.

**A server race without `--relay` had never sent anything at all.** The lobby's
addresses were written down and never resolved, and nothing marked a peer known,
so `gs_wire_send` walked a list of peers it considered unknown and sent to none
of them. Silently. Every existing test of a server race had asked for the relay,
so nothing said so. There is now a test that races two clients the server
introduced, directly, and it fails without the fix.

**Only the welcome populated peers.** A client welcomed before anybody else
arrived is told about nobody, and the lobbies that follow are how it learns who
turned up — so the first player in every race could not see the second. Peers
are now taken from every lobby.

**Readiness meant introduced, not able to race.** That was the same thing while
the mesh was in the clear. It is not any more: a race that started on addresses
alone began before the peer tunnels existed, so the first ticks of input went
nowhere — and they cannot be recovered, because the redundancy in each datagram
only reaches back thirty-two ticks. The race then ran to the end and confirmed
nothing, which reads as a desync and is a race that started too early. Readiness
is now decided in one place, last in the poll, and includes a sealed channel to
every player.

That "in one place, last" is itself the fix for a bug: the first version worked
it out in the middle of the poll and the roster arrived later in the same poll
and wrote the old answer over the top.

#### And one in the key distribution

The host sends the roster again every time anybody joins, so the early ones
carry blank entries for the seats still to be filled. Taking a key from one of
those meant remembering thirty-two zero bytes as that player's identity for
ever — a key already known is deliberately not replaced — and every later
handshake with them then failed its check, silently. Three machines out of four
raced hearing only the host. An empty slot is somebody who has not arrived, not
somebody with no key.

#### What is left in the clear, and why it has to be

The handshake messages themselves. A key exchange cannot be encrypted under a
key that does not exist yet; that is what a key exchange is for. IK is chosen
partly because it encrypts the *client's* identity inside its first message, so
what a passive observer learns is that somebody spoke to this server, and not
who.

### A track has an owner, and the ones that shipped have none

**Ownership is a key, not a name.** It used to be the author string — whatever
the uploader typed — so "only the person who put it up may take it down" meant
"only somebody willing to type the same word". The owner is now the static
public key the client proved it holds during its handshake, which is a thing
nobody else can present. There is a test with two clients both calling
themselves ada, in which only one of them built the track, and only that one can
take it down.

That check was impossible to write before the tunnel. The server had nothing but
a name, and a name is a claim. This is the first thing in the project that is
built on knowing who somebody actually is.

**A track that shipped with the game is outside all of it.** Not an owner nobody
got round to setting — outside it. `shipped` says so in the row rather than
leaving it to be inferred from a null owner, and every write path refuses a
shipped track whoever is asking, including a profile that happens to be called
the same thing as its author. `gearstick_make_store` marks them as it builds the
library.

**Private, shared with named people, or public.** Sharing names the person by
the public key the server watched them prove, which a client reads out of the
lobby — so a track is handed to somebody you are in a room with, rather than to
a string you typed. A shared flag with no list would be shared with anybody who
asks.

The listing and the visibility check are the same SQL, written once and used
twice. Two pieces of SQL that disagree about who may see what is exactly how a
private track ends up in somebody's list.

#### The schema needed a real migration

The previous items all got away with `CREATE TABLE IF NOT EXISTS`, because a new
table is created and an old database simply gains it. Columns are not like that:
the statements do nothing to a table that already exists, and the library
committed under `assets/` already exists. Every query naming a new column would
have failed on the one database that ships and passed on every test that made
its own.

So columns are added with `ALTER TABLE`, ignoring the "duplicate column name"
that a fresh database answers with, and each default makes an old row mean what
it always meant: a track nobody owns, that did not ship. The old `published`
flag is then translated into the new visibility, because leaving the two to
disagree is what a migration is for.

#### What is not there

**No delete over the wire.** The store has `gs_store_delete_track` and it is
owner-checked and tested, but no protocol message reaches it — so no client can
delete anything, which satisfies the "cannot delete somebody else's" half by
there being no delete at all. Worth saying plainly rather than counting it as
done.

#### Three CI failures, all from this phase

**Windows could not find `ext/libsodium`.** That job spells its submodule list
out inline rather than using `$CI_SUBMODULES`, because the shell it runs under
does not expand it — so adding libsodium to the variable reached every job
except the two that matter for Windows. Both inline lists now carry it and say
why they are written out.

**The fuzz job asked apt for `clang` and got 18**, which is below this project's
C23 floor. The configure said so and stopped, which is the gate working rather
than something to route around; it asks for clang-19.

**The stock library no longer matched its own rebuild.** `gearstick_make_store`
marks its tracks shipped now, so the committed database and the one CI builds
from source had diverged — which is exactly what that job exists to catch. It
was regenerated with the pinned SQLite amalgamation, as CI does, and checked
byte-identical across two runs.

### A profile you can prove is yours

Ownership is already a key, but a key belongs to a *machine*: reinstall and it
is gone. A password is what makes a name yours across machines, and it is the
first thing here that a person rather than a device can hold.

**This is small only because the tunnel came first.** Inside a sealed channel a
password can simply be sent and a code can simply be quoted. Before it, both
would have needed a challenge-response construction built for the purpose —
exactly the kind of thing this project refuses to invent — and the item would
have been several times the size for no more security.

The password hash is libsodium's `crypto_pwhash_str`: Argon2id, with the salt
and the cost parameters inside the string it returns, so there is nothing to
store alongside it and nothing to get wrong. Two hashes of the same password
differ, which is what stops a stolen database saying which two people chose the
same one. Verifying hands the whole comparison to the library that owns the
format, because comparing strings leaks how much of a password was right.

**The store holds; `gs_auth.c` decides.** What the database keeps is an opaque
hash and an opaque secret; every question about whether a password is right or a
code is current is answered elsewhere. That split is why the store still links
no cryptography.

#### The second factor

RFC 6238 over RFC 4226, on libsodium's HMAC. **TOTP-SHA256, not SHA-1** — RFC
6238 names all three of SHA-1, SHA-256 and SHA-512, and libsodium ships the
second and third and not the first. Choosing the one the audited library
actually has beats implementing SHA-1 here to match what most phone apps default
to, and it is said out loud rather than discovered by somebody whose
authenticator gives them six wrong digits.

It is checked against the RFC's own published values — the SHA-256 column, at
this project's six digits rather than the specification's eight, which is why
they are the last six of the published numbers — and against Python's `hmac`,
which is a different implementation by different people. Both agree.

**A code works once.** The window exists because two clocks are never exactly
together and a second factor that refuses a phone eleven seconds fast is one
nobody can use; but a window without a spend is a window in which a code works
more than once. The time step is retired in the same statement that checks it,
for the same reason the session nonce is.

Every candidate step is tried even after one matches, so how long the check
takes does not depend on which step was right.

#### What a player can actually do

A name nobody has claimed can be claimed, with a second factor for anybody who
wants one — the client generates that secret, because a second factor whose
secret the server chose is one the server could use. A name already claimed can
only be re-passworded by whoever has proved it.

**A name with no password still just works.** This is the case that must never
break: a racing game that demands an account before anybody can drive has lost
the argument. And somebody who joins under a name that is spoken for lands under
a name of their own rather than being thrown off — being kicked for picking a
taken name is a worse experience than being told it is taken.

#### Four CI failures, and what each was really about

**Windows, twice, and the second time was my fault for fixing the first one too
narrowly.** `gs_blake2s.h`, `gs_noise.h` and `gs_auth.h` each listed
`<stdbool.h>`, `<stddef.h>` and `<stdint.h>` by hand, on the reasoning that they
should depend on nothing of ours. MSVC's C23 is partial: `bool` needs the header
and `nullptr` needs a shim, and `gs_common.h` is where this project already
keeps both, with the shim switched on by a configure-time probe rather than
guessed from a version number. Fixing `bool` in one file without asking whether
the same class of problem applied elsewhere cost a whole round trip. There is
now a local check — compiling with `-std=c11 -DGS_NO_NULLPTR` reproduces the
condition without waiting for a Windows runner.

**macOS timed out, then failed.** The timeout was honest: this phase added six
server tests, each starting a real server and talking over real sockets, and
ninety seconds here is over three minutes on a loaded runner. The failure
underneath it was a test that pumped a fixed number of times and then asked
whether a share had landed — fast enough here, early there. It waits for the
answer to become what it should be now, which is the discipline the roster tests
already learned.

**The noise fuzzer was never built.** It was added to CMake and not to the list
CI builds, so `ctest` ran a binary that did not exist. The error said "no such
file or directory" the whole time and nobody could see it, because
`RunFuzzer.cmake` discarded the runner's output. What a fuzzer prints when it
dies is the entire report; it is captured and printed now, and that change paid
for itself twice in one evening.

### The transport, written down

`docs/TRANSPORT.md` says what goes between a client and a server, byte for byte:
the six-byte envelope, both handshake messages with offsets, the sealed
datagram's counter and ciphertext, the key schedule, the nonce layout, the
replay window, and the message limit.

**The half that is worth more is section 11, which is what it does not claim.**
No denial-of-service resistance — anybody who can send datagrams can make the
server do X25519, and that trade was made knowingly for a game server. No
post-compromise security, because there is no rekey and no ratchet. No
protection of the server's key at rest. No traffic-analysis resistance. No
formal proof of this implementation, only published vectors and an independent
implementation, which is a weaker and different statement. A reviewer needs to
find those out at once rather than by looking for them.

Two things in it are the ones an implementer is most likely to get wrong, so
they are called out rather than left in a table: the protocol name is 33 bytes
and therefore **hashed** rather than padded when the symmetric state is
initialised, and HKDF uses BLAKE2s's **64-byte block**, not its 32-byte digest.
Either mistake produces a key schedule that is self-consistent and matches
nobody.

The byte sizes the document quotes are pinned by a test. A document that drifts
from its code is worse than no document, because it reads as authoritative while
being wrong.

**Not finished, and the reason is not the document.** The item's verification is
that somebody who has not read the code can implement a client from it and
complete a handshake. The person who wrote the code cannot be that person, so it
stays unticked. The nearest evidence that exists is `tools/noise_interop.py`,
where an implementation nobody here wrote completes a handshake in both
directions — but that is a library implementing the Noise framework, not
somebody implementing *this document*.

### An installer for Windows

CPack's WiX generator, alongside the zip rather than instead of it. On Windows
an application people actually install is an installer; somebody who would
rather have a folder still gets one, and the zip needs no administrator and
leaves nothing behind by construction.

**The upgrade GUID is fixed for ever.** It is how Windows knows that 0.2 is the
same product as 0.1 rather than a second copy to install beside it. Generating
one per build would leave somebody with every version they ever installed still
on the machine, and there is no way to fix that afterwards for the people it
happened to.

Only the game gets a Start Menu entry. The headless driver and the server are
command-line programs, and a shortcut that opens a console window and closes it
again is worse than no shortcut.

#### What CI checks — including the machine

**"A machine that has never had a toolchain" turned out not to need a person.**
It was filed under things only a human could do, and the Linux job had been
doing exactly this with a Debian container all along; nobody had asked whether
the Windows side could. A Windows Server Core container has no Visual Studio, no
CMake, no SDL and no redistributable, which is what a player's computer is.

The container check refuses to trust itself first: finding `cl.exe`, `cmake` or
`VCRUNTIME140.dll` fails the run rather than reporting a pass that means
something weaker than it sounds. The image is chosen from the host's build
number, because process isolation needs them to match and a pinned tag rots when
the runner image moves.

#### And on the runner

The packaging job installs the MSI with `msiexec /qn`, **finds where it landed
rather than assuming the path**, checks the assets went with it, runs the
installed headless driver against the golden replay, checks a Start Menu
shortcut exists, uninstalls, and then checks that both the program and the
shortcut are gone. An installer that cannot be removed is one people are right
to refuse.

**Two things it does not establish**, and the plan says so rather than counting
them:

- The runner is the machine that just built the thing, not one that has never
  had a toolchain. What the check does cover is the part that is about the
  installer — that it installs, that what it installed runs, that removing it
  removes everything — rather than the part that is about the machine.
- The shortcut is checked for existence, not launched. "The game runs from the
  Start Menu" is verified as far as an unattended machine can and no further.

Both of those need a person with a clean Windows box, which is the same shape of
gap as listening to the sound on three platforms.

### The Windows build wanted a redistributable nobody was going to have

Not on the plan, and it would have shipped.

MSVC links the dynamic C runtime by default, so `gearstick.exe` needed
`VCRUNTIME140.dll` and `MSVCP140.dll` from the Visual C++ Redistributable. Every
machine that has ever tested the zip has Visual Studio on it — that is what a
build runner is — so it always worked. A player without it gets a missing-DLL
box and no game, and **a zip cannot fix that even in principle**, because a zip
cannot install a redistributable.

The runtime is linked statically now, set before anything is added so SDL,
SQLite and libsodium are built the same way; two halves of one program
disagreeing about the runtime is a link error at best. The packaging job runs
`dumpbin` against the *installed* executable and refuses anything importing
`VCRUNTIME`, `MSVCP` or `api-ms-win-crt`.

It was found by asking what the plan's "a machine that has never had a
toolchain" clause was actually about, rather than filing it under things only a
person can check. Most of what that clause is worried about on Windows is
exactly this, and exactly this is checkable from here.

### The front end had no idea who was playing

The game opened on a title screen and started racing. Records were attributed to
whichever roster slot the setup screen happened to be pointing at, and **a client
started with `--server` never showed a front end at all** — `--online` was in the
list of things that skip the menu, alongside `--shot` and `--showroom`, which are
machines being told exactly what to draw. So the one case where several people
share a server and it matters whose lap time this is was the one case that never
asked a question.

There is a door now, and it is the screen the game starts on.

- **A driver can carry a password and a second factor.** `gs_profile` gained an
  Argon2id encoded hash and a TOTP secret, taking the format to version 3;
  versions 1 and 2 still load, with everybody in them unlocked. Core stores them
  as opaque bytes and understands neither, because `src/core/` links nothing and
  libsodium is a something — the hashing is `src/net/gs_auth.c`, which the game
  now links. The two headers cannot disagree about the field sizes: a
  `static_assert` in `gs_auth.c` pins them, since core cannot include the header
  that knows.
- **Every driver has a password.** A driver carrying none cannot be signed in —
  not because the empty string fails the check, but because there is nothing to
  check against, and a door that opens for anybody with no key is a picture of a
  door. This replaces an earlier decision that an unlocked driver was fine; it
  was not what was wanted, and "one person on one machine" is not the only
  machine this runs on. A roster written before version 3 is not turned away:
  its drivers are offered a password on the way in, since having played the game
  before it had a door is not a thing to be punished for.
- **The check is in one place.** `gs_menu_sign_in` is the whole rule, and the
  gate that forces the login screen sits at the top of `gs_menu_frame` rather
  than in each screen — a check every screen has to remember to make is a check
  one of them eventually will not. It is public so it can be tested without an
  ImGui context, because a rule that can only be exercised by clicking is a rule
  nothing checks.
- **What was typed does not linger.** The password is wiped the moment it has
  been checked. It survives only as a copy the frontend takes exactly once, to
  prove the same name to a server over `GS_MSG_LOGIN` — which the wire has been
  able to send since the server was written and which the game had never called.
  Taking it is what wipes it.
- **The menu is play, tracks, profile and exit.** Play goes to the track chooser
  first and the settings after, the way *Racing Destruction Set* did. Exit is a
  flag the menu raises and the frontend acts on, because the menu does not own
  the loop.
- **Signing in lands on the menu, and only the lobby joins a race.** Online,
  **Play** is what puts you in the queue — the track, the roster and the moment
  it starts are the server's to decide, so Play goes to the lobby rather than
  offering a local track chooser. Until then an online player can read the
  records or go and build something with the race waiting.

The tests name the facts: `a_password_on_a_profile_is_actually_required`,
`a_second_factor_is_asked_for_when_the_profile_has_one`,
`the_front_end_is_shut_until_somebody_signs_in`, and, in the simulation tests
where the format lives, `a_roster_written_before_passwords_existed_still_loads`.
The password test was checked by defeating the password check and watching it go
red, because a test that passes when the thing it guards is removed is not
evidence.

The golden replay is untouched, as it must be: none of this is simulation. A
colour cannot change where a car ends up and neither can a password.

**The first version of this shipped a bug into the very thing it was building.**
Signing in sent an online client to the lobby, and a one-player server is ready
the moment it is asked — so pressing SIGN IN started a race instead of showing
the menu. It was found by playing it, one commit later, which is the only way it
was going to be found: every test passed, because the rule lives in the
frontend's main loop and the frontend links a window system that no test
executable stands up.

So the check is the real programs. `tools/front_door_check.py` starts a real
server, points a real client at it, and checks **both** directions — a client
left on the menu must still be there, and a client left in the lobby must get
in, because "the menu did not start a race" would pass just as well if nothing
ever raced. It runs as `gearstick_front_door`, redirects the store into a
temporary directory so it cannot touch the profile somebody plays with, and was
confirmed by putting the bug back and watching it go red.

### The caret nobody could see

Dear ImGui draws the text cursor with `ImGuiCol_InputTextCursor`, not
`ImGuiCol_Text`. `gs_base_palette` is a fixed list of colours copied from
upstream and written before that colour existed, so the slot kept whatever
`ImGui_CreateContext` left in it — the dark theme's white — and the inversion
that turns this palette dark then turned white into **black**. A black caret on
a black field: a text box with no way to tell you were typing into it.

That is the trap in writing a palette as an enumerated list. A colour added to
the library later is silently inherited and then transformed by a rule that was
never meant to apply to it, and nothing warns you, because every colour has a
value.

It is pinned by a test that renders an empty box with and without the keyboard
and counts the pixels drawn inside it. The caret is the only thing that can be
in an empty box, so the count *is* the caret. The test reproduced the fault
before the fix — it was written to fail first, and did.

The lesson worth keeping is about where the gap was: the rule that decides
whether somebody is in a race was in the one file the test suite structurally
cannot link, and nothing noticed until a person pressed the button.

### A server that stopped serving because of its own output

The front door check went green on Linux, Rocky and macOS and red on Windows,
every time, from the commit that introduced it. The client's log was three
startup lines and then thirty seconds of nothing: it announced the server it was
meeting, printed the assets directory and the track hash, and never heard back.

The server was blocked inside `printf`.

`gs_draw` repaints a dashboard four times a second — six hundred bytes of table
— and the check ran the server with its output on a pipe, read it until the key
line and then never read it again. A pipe holds what a pipe holds; once it was
full the next write blocked, and a server blocked in a write is not draining its
socket, not answering a knock and not sending a track. It had stopped being a
server on account of its own output.

The platform split is the pipe size and nothing else. **Windows `CreatePipe`
takes a four-kilobyte default buffer, which is about one second of dashboard;
Linux gives a pipe 64 KB, which is sixteen seconds.** The check spends its first
eight seconds proving that a client on the *menu* does not get pulled into a
race, and then has its lobby client join at about the nine-second mark — inside
the sixteen, outside the one. The Linux pass was luck with a stopwatch on it.

It reproduces on Linux by taking the luck away: leave the pipe unread for
twenty-five seconds and then join, and the client sits there exactly as it did
on Windows, with the same three lines and nothing after them. With the fix in,
the same run gets `net: 1 players, driving car 0`.

**The dashboard is now for a terminal, and everything else gets a log.**
`isatty` on stdout decides which: a terminal gets the four-a-second repaint, and
a pipe or a file gets the event lines it was already writing plus one line a
minute saying how long the server has been up and who is on it. Sixty bytes a
minute cannot fill a pipe this decade, and a log of what happened is what
somebody redirecting a server's output wanted in the first place — nobody has
ever wanted fourteen thousand copies of a table. `--plain` keeps its old meaning
of "no cursor control", which is now only about dumb terminals.

Fixing that uncovered a second fault underneath it, which is the reason the
first one was ever survivable: **stdout is fully buffered when it is not a
terminal**, so the `key` line — the one a client cannot connect without, because
IK means the client has to know who it is talking to — was sitting in a
four-kilobyte buffer. It had only ever been visible because the dashboard was
flooding that buffer several times a second. Take the flood away and the key
never arrives; the check hung waiting for it. So stdout is unbuffered from the
first line of `main`. `setvbuf` with `_IOLBF` would do on Unix and would not on
Windows, where the runtime treats line buffering as full buffering, so `_IONBF`
is the only spelling that reaches a reader everywhere.

The check that pins it is `tools/server_output_check.py`, and it states the rule
in the units that caused the bug. It runs the server with `--seconds 5` and its
output going into a pipe nobody reads, requires it to stop when it was told to —
blocked in a write it would never reach its own clock — and then requires what
it wrote in that time to be **less than a pipe holds**, taking 4096 bytes as the
smallest buffer any of the three platforms gives us. The old server writes about
11,800 bytes in those five seconds and fails it on every platform, including the
ones where the fault was invisible; the new one writes 572. There is a third
assertion, that the key was announced at all, because "wrote almost nothing" is
also what a server that never started looks like.

`tools/front_door_check.py` now reads the server's output the whole way through
on a thread of its own, and prints **both** logs when a half fails. The server
no longer writes enough to fill a pipe, so the drain is belt and braces — but a
harness that leaves a child's pipe unread is a trap set for whoever writes the
next one, and the missing server log is why this took an afternoon rather than
ten minutes: the client's log says only that nobody answered, which is equally
what a broken client, a broken server and a blocked server look like. The same
change took out `os.set_blocking`, which only learned about pipes on Windows in
Python 3.12; a blocking read on a thread behaves the same everywhere.

The lesson worth keeping: **the tripwire that found this was a test of something
else entirely.** No unit test can catch a server that blocks on its own stdout,
because a unit test does not run the server as a process with a pipe on it. It
took the front door check — two real programs, a real socket and a real pipe —
and it did not so much fail as reveal that one platform had been passing for a
reason nobody had checked.

### And the same check was red for a second reason underneath it

With the pipe fixed, the Windows front door check failed again — and this time
the server's log, which the harness had just learned to print, said what
happened in two lines:

```
player 0 (tester) joined from 127.0.0.1:53443
player 0 (tester) left: went quiet
```

One join, for two clients. The check runs two halves against one server that
holds a single slot: a client on the menu, which must *not* be pulled into a
race, and then a client in the lobby, which must be. **On Linux, killing the
first client gets it a signal it can act on, and it says goodbye on the way
out**, so its seat is free by the time the second knocks. On Windows,
`terminate()` is `TerminateProcess`, which is not a signal and cannot be acted
on: the client vanishes, the server keeps its seat for the full fifteen-second
timeout, and the second client is told the server is full. A refused client does
not knock again — that is deliberate, and the reason is in `gs_wire_poll`: a
full server does not become less full for being asked twice.

So the check was failing for something that had nothing to do with the rule it
exists to pin, and it was passing on three platforms for a reason it did not
have on the fourth.

Reproduced on Linux by taking the goodbye away — `kill` instead of `terminate`
on the first client — which produces the Windows log exactly, down to the "went
quiet". Two changes fix it, and the second is the more important one:

- **A server each.** The two halves are independent scenarios and had no
  business sharing a lobby; sharing one made the second half depend on the
  first's teardown.
- **Both clients are killed outright, on every platform.** A harness whose
  teardown is graceful on one platform and abrupt on another has a platform
  difference baked into it, and this check has now been bitten by exactly that
  twice in one afternoon. Both ends do the harsher thing, so a pass on Linux
  means what it says about Windows.

### Panels that had outgrown their windows

Three things found by photographing every front-end screen and looking at the
pictures.

**The library screen was taller than the window.** Its panel was sized to hold
the whole library — `340 + row * (tracks + 1)` — which is 869 pixels for the
twenty-two tracks that ship and 1,099 for the thirty-two a library can hold.
Centred in a 720-pixel window, that puts the title bar and the first track
*above the top edge*, and these panels are `NoMove | NoResize`, so what is up
there cannot be reached at all. **The first track in the library could not be
chosen**, and the more tracks somebody built the more of them went with it.

**The title screen and the drivers screen were the same fault from the other
end**: a panel at a height worked out by hand, with content that had grown past
it since. The title screen hid fifteen pixels — the bottom of the `sign out`
button and the status line under it — and the drivers screen hid forty-three,
which was its `back` button. Neither is visible in a screenshot as anything but
a slightly odd-looking panel, which is why both survived a screenshot test.

The fix has three parts:

- **`gs_centre_window` clamps to the window.** A panel is never wider or taller
  than the viewport less a small margin, so the library screen is now 700 tall
  in a 720 window instead of 1,396, and what does not fit scrolls — reachable,
  rather than drawn where the mouse cannot go.
- **The library list scrolls inside its own box.** The space left over after
  everything else on the screen decides how many rows are on view; the header
  row is frozen so a scrolled list still says what its columns are. A library is
  the one thing on any of these screens with no upper bound worth designing
  around — the whole point of the editor is that it fills up.
- **The two hand-set heights were raised** to fit what is now in them, which is
  a fix that goes stale again the moment somebody adds a control. That is what
  the test is for.

`no_screen_is_drawn_bigger_than_the_window_it_is_in` draws every screen — with a
driver signed in, a full library, a finished race and a lobby with four people
in it, because a screen measured with nothing in it is measured at its smallest
— and checks two things at the size the game opens at: the panel lies inside the
window, and nothing is below the fold. Then it halves the window, which is what
dragging a corner does, and checks the first of those again: what does not fit
must scroll rather than be drawn out of reach.

The measurement comes from the menu itself. Every screen ends with
`gs_panel_measure`, which notes where the panel came out and what
`ImGui_GetScrollMaxY` says is hidden below it, and the test reads that rather
than counting pixels. It failed on five of the eight screens before the fix,
which is the only reason to believe it is measuring anything.

### And the line that says you set a record read "lap + race r"

Found in the same pass, by racing a whole session and photographing what came
out of it. The results table's last column is the note that says which records
the drive took, and it was a stretch column — it got whatever was left after
five fixed ones, which was eighty pixels for a sentence that wants a hundred and
thirty. A table wider than the window it sits in does not overflow; the last
column gives up what is missing. The one line telling somebody they have just
beaten a record was the line being cut in half.

The column is now as wide as the widest sentence that can go in it, asked of the
font rather than counted by hand, and the panel is worked out from what the
table needs: the five columns, the note, the padding a cell puts on either side
of each of them, and the window's own margin. Nothing there needs re-tuning when
the font changes. The same string the column is measured from is the string that
gets drawn, so the two cannot drift apart.

### And the construction set's gravity dial was labelled "gravity (x Eart"

The third of the same kind, in the other half of the product. Dear ImGui puts a
widget's label to its right and clips whatever does not fit, and the palette is
340 pixels wide by default - so the one dial whose units matter had its units
cut off. Every slider in the palette now stops where the longest label starts,
that width being asked of the font across all nine of them, so they fit at
whatever width somebody has dragged the palette to. They also line up in a
column now, which was not the point and is an improvement anyway.

**All three are the same mistake**: a width decided by hand, and text measured by
eye against it. The cure in each case is to ask the font how wide the words are
and let the layout follow, which stays true when the words change.

### Four things found by playing it against a real server

A player signed in, pressed PLAY, and got a race that froze after two seconds
with the controls dead. Four faults, three of them serious, and no test in the
tree would have found any of them.

**Nothing anybody did had ever been saved.** The client's log carried
`store: could not write .../gearstick.store:` with an empty reason after it, over
and over, and the disk was fine. `gs_store_save` sized its buffer as the roster,
the records and four kilobytes of slack - and the slack has to hold the
*library*, which is about four kilobytes for one track and ninety for the
twenty-two that ship. So `gs_menu_save` refused every time it was called, on
every machine, from the moment the library joined the store. The message blamed
the disk because it printed `SDL_GetError()` on a path where SDL had not been
asked to do anything: the "could not build it" case and the "could not write it"
case shared one line. The frontend now asks `gs_menu_size` how many bytes this
store takes and holds exactly that, and the two failures say different things.
`a_store_with_tracks_in_it_is_saved_whole` builds its library out of the tracks
that ship - a track built in a loop is flat, compresses to nothing and would fit
in any buffer, which would have made the test pass while saying nothing - and
pins both halves: a buffer that does not fit is refused rather than
half-written, and the size that is asked for is enough.

**A race with nobody else in it froze after 2.12 seconds.**
`gs_advance_confirmed` was called from `gs_net_receive` and nowhere else, so
confirmation depended on somebody writing in. In a one-player race nobody ever
does: nothing was confirmed, the 256-tick window filled, and the stall that
means "the other machine has gone quiet" fired forever at a race that had no
other machine in it. Tick 255 at 120 Hz is 2.125 seconds, which is the number
the HUD was stuck on in the screenshot. `gs_net_step` now confirms what this
machine already knows, which walks forward only over ticks whose inputs are
*all* known - so a race with company still waits for the other machine's reveal,
and `a_machine_that_goes_quiet_stalls_the_race_rather_than_desyncing` still
passes. `a_race_with_nobody_else_in_it_never_stalls` drives three windows solo
and fails on the first one without the fix.

**An online race was built from the local setup screen.** Before the front door
existed an online client skipped the menu, so `gs_start_race` took its
`skip_menu` branch and built the grid from the wire. Phase 16 sent online
clients through the front end and nothing moved the race-building with them, so
the setup branch ran instead: the car count, the machines, the paint, the
gravity, the laps and the mode all came from *this machine's* setup screen. On a
one-seat server that put two cars on the grid, and the phantom sitting on the
start line is what pulled the camera off the player's own car. In a real
two-player race it is worse than untidy: two machines would build different
worlds from their own screens, which is the one thing rollback cannot recover
from. An online race is the server's race again - as many cars as the server
says are playing, on the stock machines, with the world's own dials. The client
now says the grid it built as the race begins, and `tools/front_door_check.py`
refuses a race whose grid is not the size the server said; on the old code it
fails with "the server said 1 player(s) and this client built 2 car(s)".

**The condition bar ran into the edge of the HUD.** It was drawn `GS_HUD_W - 16`
wide - eight pixels of padding a side - and the HUD is drawn in the menu's
style, which pads a window by twenty-two. Nothing was drawn outside the panel,
because ImGui clips; what happened is that the bar ran up to the frame and sat
against it with no margin. It asks for the content region it is in now. The test
counts the bar's green in the last ten pixels before the frame, where finding
any is the fault.

Two of the four are the morning's mistake again - **a number written down by
hand about something that is measured somewhere else**: eight pixels of padding
where the style says twenty-two, four kilobytes of slack where the data is
ninety. The other two are what happens when a feature moves and the things that
depended on where it used to be do not move with it.

### The race with no car in it, and what was built so it cannot happen again

The next race showed the track, the HUD, a clock that ran - and no car anywhere.
The camera was, in x and y, exactly where the car was.

**The race camera pinned its height to zero.** `gs_split_update` set
`shared.cz = 0.0f` and the split panes did the same, while the camera that snaps
to the grid at the start of a race - `gs_render_track_camera` - followed height
at 0.35. So the first frame was right and every frame after it was wrong, by as
much as the ground was high: on a track whose start line sits eight tiles up,
the car is drawn eight tiles up, which at the default zoom is three hundred and
eighty pixels above the middle of a 720-pixel window. Off the top. On flat
ground at height zero - the demo track, and every screenshot anybody had ever
taken - it looked perfect.

The fix is to say what the camera is actually for: **it follows the ground
fully and the air only partly.** `gs_cam_height` is the ground under a car plus
`GS_CAM_FOLLOW_Z` of however far above that ground it has got, and all three
cameras use it. A track built up in the air is centred; a car in a jump climbs
its own screen away from its shadow, which is the most readable thing in the
frame and the reason the partial follow existed in the first place. The two
tests are the two halves of that sentence: every driver can see their own car on
ground that is not at height zero, for one to four cars, split and merged; and a
car in the air is higher up its own screen than one on the ground and still on
it.

**A wrecked car had nowhere to go.** A wreck ends nothing - the race waits for a
finish that is never coming - so nothing moves the player on, and Escape led to
the setup screen, which decides a race a server owns. Where "back" goes is now
`gs_menu_back`, a rule with a test rather than four lines in a key handler that
no test can reach: out of a race is the setup screen on this machine and the
lobby when the race is somebody else's, everything else backs out to the title,
and the title and the door are where leaving belongs. The HUD says `Esc leaves`
when the car is wrecked, and the panel is sized for that line rather than
hoping.

**And the thing that stops this happening again.** Every fault found by playing
this has been in the thirty seconds after the green flag, and nothing was
looking there: the front door check proves a client gets *into* a race and then
stops watching. So the client can now be driven and can now report itself:

- `--autodrive` puts the AI at this machine's wheel through the ordinary loop -
  input, network, camera and all - rather than the session's straight-line
  simulation, so what is exercised is the client and not the physics that
  already has tests.
- `--trace` prints a line a second in key=value: the screen, the tick, which car
  is this machine's, where it is, how fast, whether it is wrecked, **whether it
  is on this machine's screen**, where it was drawn, where the camera is, and
  how many times the rollback has stalled.
- `--track FILE` opens a particular track, so a check can race ground that is
  not at height zero.

`tools/play_check.py` races the game twice - on this machine and against a real
server - and asserts what a person checks in the first five seconds without
noticing they are checking: a race starts, the clock advances and keeps
advancing, the car is on screen every single look, it gets somewhere, and
nothing stalls. Every one of those was false today on a build whose tests were
all green. Run against the old camera it fails with the whole diagnosis in one
line: *the car was off the screen on 8 of 17 looks - at tick 0 it was at
3.00,5.50 drawn at 640,-24 with the camera on 3.00,5.50*.

### Two more the trace found, in the same afternoon

With the trace in, the next two took minutes rather than hours - which is the
argument for having built it.

**A car wrecked in the air stayed above the top edge for the rest of the race.**
A car that goes over the drop is stopped where it is rather than at the bottom,
because what a player needs to see is where the mistake ended - so it can hang
eight tiles up for good. Following 0.35 of the air is right for a jump, which
lasts a second, and wrong for a car that is airborne permanently: it sat off the
screen and stayed there. `gs_cam_hold` caps the follow at a third of the pane,
in tiles worked out from the pane's own height and zoom, so the car is always
inside its own view - and still higher up it than one on the ground, which is
the half of the rule worth keeping.

**And two cars far apart in height counted as together.** The split screen asked
how far apart the pack was in x and y alone, so a car twenty tiles down a drop
and a car still racing were "close", the screen stayed merged, and the shared
view framed the empty air between them. Height is part of how far apart two cars
are now, which splits the screen for a pair that cannot be held in one view -
and the cap above keeps each of them inside their own pane once it has.

Both are pinned by tests that fail without them: a car down a drop does not take
the camera off the other one, and a car in the air climbs its own screen rather
than leaving it.

### What was photographed, and what it showed

Every screen and every mode, captured headless and looked at: the eight
front-end screens, races on seven tracks including ice, high gravity and two
jump tracks, one to four cars, split and merged, the landing arc, the gravity
overlay, the construction set, the analyser's heatmap, the showroom, a wreck,
and a race against a real server. **And then the camera invariant over all
twenty-two shipped tracks** - each raced by the AI for half a minute with the
trace on, checking that the car is on screen at every look.

Two things worth writing down from that pass. `--players` had been doing nothing
since the front door landed: `gs_start_race` ran at start-up *before*
`skip_menu` was decided, so it always took the menu branch and built the setup
screen's two cars. Four-player split screen was unreachable from the command
line while the help text offered it, and every capture anybody had taken of a
"four player" race was a picture of two. The decision now happens before the
world is built, and a grid asked for on the command line is also what the setup
screen offers, so the two cannot disagree.

The other is that the showroom draws a HUD for a car nobody is driving, which is
harmless and looks odd. Left alone deliberately: it is a screenshot mode.

### A dead driver, told so

Dying said almost nothing. The word "WRECKED" replaced the word "condition"
under an empty damage bar, and the lap clock kept counting - so a car that had
been dead for a minute and a half read as somebody on a very slow lap, which is
the opposite of what had happened. There was nothing to press, either: a wreck
ends nothing in the simulation, so nothing moves the player on.

The HUD says **YOU DIED** where the bar's label goes, stops the lap clock at a
dash, and names the two keys that do something about it: `R restart` on this
machine, `Esc menu` - or `Esc lobby` when the race belongs to a server, where
restarting is not one machine's to do. The time the wreck happened is not shown,
which would be nicer: the simulation does not record it, and adding a field to
the car to carry it would change the world hash, and with it every replay,
ghost and shared time in existence, for a line of text.

The same panel now says when the race is **waiting** for another machine and for
how long, because a rollback that stops the world while a peer is quiet is
indistinguishable from a crash if nothing says otherwise - which is exactly what
a player saw after pressing Play into a race whose second seat had been
abandoned. And the waiting ends: after twenty seconds the race is over and the
results screen appears. Twenty because the server drops a silent client at
fifteen, so a client merely having a bad moment is dropped by the server first
and this only fires for somebody genuinely gone.

**Sizing that panel by hand had been quietly wrong all along.** The height was a
sum of scaled rows, labels and a guess at the gaps, and it was eleven pixels
short in *every* state - the bottom label was clipped and nobody noticed,
because it is short. Adding the wreck message made it a whole line short. It is
worked out from the rows it is about to draw now, in the order it draws them,
including the four deliberate `Spacing()` gaps that cost a gap each and were
never counted. `gs_hud_overflow` reports what did not fit, and the test renders
every state the HUD has - racing, wrecked, waiting, finished and the
combinations - and requires nothing below the bottom in any of them.

### A track that says where it ends and which way it goes

A player raced *the long drop* - "a shelf that ends" - drove straight off the
end of it, and asked why they kept dying there. The trace answered it exactly:
from the start line to the wreck, `y` never changed, and the wreck was at
x=61.01 on a track 48 wide. Thirteen tiles past the edge is the run-off plus the
fall depth, which is the "you are gone" rule. They drove off the end because
**nothing on the screen said where the end was**.

Two things were missing, and both had been missing since the renderer existed:

**The edge of the road.** The only thing distinguishing the authored track from
the run-off was a 0.55 multiplier on the tile's shade - which is legible in a
still picture from above and useless at speed in an isometric view. It is a
kerb now: red and white blocks, one to a tile, all the way round the authored
ground, drawn on the tile at the boundary so it sorts with the terrain and
follows the ground over a rise. A racing driver has been reading that pattern
since before any of us.

**Which way round.** Gates existed in the simulation, and as a thin white line
in the construction set, and *nowhere at all in a race*. Arriving at a track
told you nothing about the route. Every gate is drawn on the ground now - a band
across it, and a large arrow through it pointing the way a car is meant to pass
- and the start and finish is chequered, because one of them is not like the
others. Drawn in the sweep at the gate's own diagonal, so a gate beyond a rise
is hidden by the rise instead of floating over it.

The test counts all three by colour: the kerb's red, the arrow's yellow, the
chequer's white. It also cost one existing test a move: `a_car_behind_a_rise_is_hidden_by_it`
counts strongly red pixels anywhere in the frame, which is what makes it strict,
and a kerb is strongly red - so its scene now sits in the middle of a track big
enough that no edge is in shot.

---

## What does not exist

**This section was years out of date and said the opposite of the truth** — no
race, no AI, no sound, no front end, no shipped tracks, all of which have been
built and ticked. A reader sent to this file to learn what works was being told
by its honest half that almost nothing did. It is rewritten here and the lesson
is in the tail that caught it: the "what works" half of a status document gets
updated because it is fun to write, and the "what does not" half rots.

- **Sound has only been listened to on Linux.** The synthesiser is
  platform-independent and the device path is not. Windows and macOS need a
  person with speakers, and no amount of measurement substitutes for that.
- **Nobody has implemented a client from `docs/TRANSPORT.md`.** The document is
  written and its byte sizes are pinned by a test; what has not happened is
  somebody who has never read `gs_noise.c` writing a client from it and
  completing a handshake, which is the only thing that proves a specification is
  one.
- **Nothing has been tagged or released.** The packaging workflow builds a
  tarball, a disk image, a zip and an installer, and attests all four; no
  version has come out of it.
- **No rekey on the transport.** A session stops sending at 2^40 datagrams —
  about 290 years at 120 a second — rather than rekeying. `docs/TRANSPORT.md`
  says so in the section on what is not claimed.
- **No delete over the wire.** A track's owner can make it private, but there is
  no protocol message that removes one. The store's delete is owner-checked and
  tested and nothing reaches it.
- **No denial-of-service resistance anywhere.** No cookie, no puzzle, no rate
  limit; anybody who can send datagrams can make the server do X25519. A
  deliberate trade for a game server, written down in `docs/TRANSPORT.md` rather
  than left to be discovered.
- **The tails below the plan.** `docs/COMPLETION_PLAN.md` keeps the found-work
  that has not been done, and it is the honest list of small things this
  document would otherwise be tempted to leave out.

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

### Cars that are drawn whole, and drawn where they are

Three separate faults were making a car look wrong in motion, reported as
"jerky", as "a rendering artefact around the car", and finally - the one that
named the cause - as **"the background comes over the bonnet every few
seconds"**. They had nothing to do with each other.

**The camera read a state the renderer never drew.** The world advances in
fixed 120 Hz steps and frames are drawn whenever the machine manages one, so
`gs_render_view` draws every car interpolated between the last two states by
`alpha`. The camera did not: `gs_split_update` and `gs_split_views` took only
the settled state. So the camera was pointed a fraction of a tick away from
what was on the screen, and the fraction changed every frame - which is a car
that judders against a world that is otherwise smooth. Both now take `prev` and
`alpha` and follow the same interpolated car the renderer draws.
`the_camera_holds_the_car_still_between_ticks` walks alpha from 0 to 1 with the
merge settled and requires the car's projected position to move less than half
a pixel; it moved with alpha before.

**A car's own triangles could not be sorted, because the meshes were solids
that interpenetrated.** Every vehicle is built from boxes and the boxes
deliberately sink into one another - the glass sits inside the cabin so the
windows show on its flanks, the sills sit inside the body, the lamps sit into
the nose. The stock car alone had thirteen interpenetrating pairs, and 47 of
its 60 faces were buried in another box. `SDL_Renderer` has no depth buffer, so
a car is drawn back-to-front by triangle depth, and a painter's sort cannot
order interpenetrating solids: which triangle won was decided by two centroids,
and the toss is thrown again every time the car turns. That is what made the
artefacts *crawl*. Both alternative sort keys were tried against a real frame -
nearest-vertex and farthest-vertex - and both are worse than the centroid.

The fix is in `tools/make_meshes.py`: only the surface of the union is emitted
now. Each box face is clipped against every other box whose interior its plane
passes through, coplanar leftovers are merged back together, and what is
written out is exactly the boundary of the union - every surface a player can
see and nothing behind one. The vehicles are unchanged to the thousandth of a
tile; the triangle counts went from 120-144 to 136-268, so
`GS_MESH_MAX_TRIS` is 512, sized from the meshes rather than guessed. It was
256, which the lunar rover's 268 would have silently truncated.

**And a car was sorted into the terrain sweep by its centre tile.** The terrain
is drawn one diagonal at a time; a car was drawn when the sweep reached
`floor(x) + floor(y)`. But a car is about 1.3 tiles long, so its nose reaches
into the tile in front of the one it is standing on - which is on the *next*
diagonal, and drawn afterwards. The ground the car was standing on was
therefore painted over its own bonnet, and because the centre crosses into a
new tile every car length or so, the overpaint arrived and left as it drove.
`gs_car_diagonal` takes the maximum over the whole footprint instead, so a car
is drawn only once every tile it covers has been. Ground genuinely in front is
on a diagonal beyond the footprint and still draws over it, so a car behind a
rise is still hidden by the rise.

`a_car_is_drawn_whole_wherever_it_sits_within_its_tile` photographs the same
car from the same distance at a tenth into its tile and at nine tenths, and
requires the two to differ by under five percent. Before the fix the straddling
car lost its bonnet, headlight, bumper and both front wheels. The test is on a
48x48 track with the car in the middle for the same reason
`a_car_behind_a_rise_is_hidden_by_it` is: a kerb is strongly red, and the first
version of this test counted kerb rather than car and was measuring nothing.

### A start line and a finish line that are different things

**"It is confusing whether beginning and end is."** The grid sits `GS_GRID_BACK`
= three tiles behind gate zero, and gate zero was the only line drawn -
chequered, a few tiles in front of cars that had not moved yet. The first thing
a player saw on arriving at a track was therefore a chequered flag line, which
is the universal sign for *finished*, and the same line was also the lap line
they had to cross to complete a lap. One line doing two jobs, and looking like
the wrong one of them.

There are two lines now:

- **A plain white start line across the grid**, just in front of where the cars
  are placed so they line up behind it. `GS_GRID_BACK` moved from `gs_track.c`
  into `gs_track.h` so the renderer draws it where the cars actually are rather
  than somewhere that looks about right - the same "one definition" rule
  `gs_track_grid` already follows for the race and the analyser.
- **The chequer stays on gate zero**, with a flag standing at each end of it.
  That is the lap line and the finish, and it is now the only line drawn across
  the road that looks like one.

**And the other gates stopped pretending to be finish lines.** A waypoint was a
solid blue band right across the road, which reads as a line you cross to finish
something - a player looking at one asked whether it was the end of the track.
They are marked at their edges now, by a pale post with a blue head at each
side, and left open in the middle the way a rally stage is. The arrow through
the gate stays, because which way round a track goes was the other thing nobody
could tell.

**The flags also had to stop being ground marks.** They are drawn in the sweep
at the tile each one stands in, not at the gate's diagonal: a flag at the near
end of the line was otherwise painted over by every tile the sweep reached
afterwards, so a line that should have had a flag at both ends was drawn with
one.

`a_start_line_and_a_finish_line_are_different_things` samples each line where it
lies rather than counting the whole frame: the band behind the gate must be
white with no black in it, and the gate itself must have both. Deleting the
start line fails it.

### A line no longer swallows the car in front of it

**"The car just went behind the finish line."** The band reaches right across
the track, so it lies on a wide span of diagonals - but a gate has only one, and
the whole band was drawn at it. Every part of the line nearer the camera than
the gate's centre was therefore painted *after* any car nearer than the centre,
and swallowed it.

Each block of the band, each piece of the arrow and each piece of the start line
now sorts on the diagonal of the ground it actually lies on -
`gs_ground_quad_diagonal`, the same rule `gs_car_diagonal` uses. A car on this
side of the line is drawn in front of it and a car beyond it is drawn behind.
`a_car_on_the_near_side_of_a_line_is_drawn_in_front_of_it` photographs a car
five tiles short of a gate's centre with the band over it and without, and
requires the line to cost the car none of itself.

### A race that begins when everybody is ready

**"Can we have a light tree at the beginning with a countdown so we get a chance
to start the race."** Until now a race simply *was*, from tick zero: the world
began stepping and the only way to know it had started was that the car under
you had begun to move. Arriving at a track meant already being late.

`gs_world.green_tick` is the tick the lights go green. Before it, every car is
stepped with **no input at all** - the physics still runs, so a car on a slope
settles onto it rather than hanging above it, but nothing anybody presses
reaches a car. That is what makes the start fair rather than a race to notice.

Three things follow from where it is armed:

- **It is set in the front end, not in `gs_world_init`.** The analyser, the AI
  sweeps and the editor's background ghost are none of them people who need a
  moment to get ready, and they are untouched.
- **Zero means never counted down.** Tick zero is not before tick zero, so every
  replay recorded before there were lights is held for exactly no ticks and
  lands where it always did. `gearstick_cli selftest --verify` confirms it: the
  golden replay hash is unchanged by this commit.
- **It is armed before the first tick**, where every machine agrees the tick is
  zero, so a networked race counts down identically on every screen without a
  message being sent. A countdown decided later, or decided by one machine and
  broadcast, would be a different world on each screen.

**The lap clock no longer runs before the flag.** A lap begins at a crossing,
and until the first one `lap_start` is zero - so the HUD was showing time
already spent on a lap nobody had started, counting up while the cars sat still
on the grid. While the hold lasts, `lap_start` is pinned to `green_tick`, so the
clock reads zero at the moment anybody can first move.

**The tree itself** stands beside the grid rather than over it - a gantry across
the road would be the one thing between the camera and the cars at the only
moment nobody may miss. Three lamps light one a second, all three go green
together at the off, and the tree goes dark a second later rather than sitting
there claiming something. The HUD carries the same count in the row the lap
clock will occupy, rather than in a row of its own: the panel is sized from the
rows it has, and a row that exists for three seconds would leave a hole in it
for the rest of the race.

`nobody_drives_before_the_lights_go_green` holds full throttle and full steering
down through the entire countdown and requires the car not to have moved a
single fixed-point unit, then requires the same input to move it once the lights
change - and separately requires a race that was never counted down not to be
held for an instant. `the_light_tree_counts_down_and_then_goes_green` counts lit
pixels at each stage and requires exactly one more lamp's worth per second, all
green at the off, and nothing lit afterwards.

### Tracks that go somewhere

**"It is useless if the tracks are not real examples... without this, the
application is useless."** That was right, and the reason is worth writing down
because it is a lesson about verification and not about track design.

**What was wrong.** The generator laid the same route on every shape it built:
one gate near the left edge, one near the right. And `gs_world_step` counted a
lap when `next_gate` wrapped to zero - that is, on crossing the *last* gate. So
on a two-gate track a "lap" was a one-way trip, and lap two meant driving all
the way back across an open field, with nothing marking the way, to a line that
was where you had started. The terrain varied; the route never did.

**Why it passed.** `gs_analyse` asks whether a vehicle can get from gate to
gate at a given gravity and prints *"the route is sound"*. That is
**completability**, not raceability, and a two-gate open field is trivially
completable - so the check was green and the green read as evidence. Nothing
asked whether the route was a loop or a path, whether the finish was a
meaningful distance from the start, or whether the route used the ground at all.
The generator's own comment said what it was doing, and it was authored as a
sprint; the simulation raced it as a circuit. Nothing exercised the pair
together. `PROJECT_STATUS.md` had already named the risk - *"the feel is
unproven... that question opens the moment there is a track worth driving"* -
and `CLAUDE.md` says a hard-coded track is a prototype that must be replaced
before anything is built on it. The placeholder stayed and a great deal got
built on it, because everything downstream could be demonstrated on a bad track
as well as a good one.

**What a track is now.** `gs_track.route` says which of two things it is, and
the two are raced differently and drawn differently:

- **A circuit** is a closed loop. Gate zero is the start **and** the finish -
  one chequered line with a flag at each end, crossed at the start of every lap
  and the end of every lap. `gs_track_finish_gate` returns zero.
- **A sprint** is a path. Gate zero is a plain white start line, the last gate
  is the chequered finish, and they are a long way apart.

Everything else on the route is a waypoint: two posts at its edges and an arrow
through it, open in the middle, because only the line you cross to finish should
look like a line you cross to finish.

**And the route is carved into the ground rather than dropped onto it.** The
terrain is laid first, then a corridor is cut along the centreline: level across
its width, following the ground along its length - so a ridge in the way becomes
a ramp up and a ramp down, which is a jump, while a car is never tipped sideways
by ground that happens to fall away under one set of wheels. The road carries
its own surface, never the same as the ground it crosses, which is what makes
the route a thing you can *see* rather than a set of markers on a field.

Three things fell out of building it, each caught by a test rather than by
looking:

- **The verge is not decoration.** Stamping the road's level straight in leaves
  a cliff wherever road and hillside disagree.
  `no_generated_slope_is_steeper_than_a_car_can_climb` caught it, and the answer
  is `gs_relax`: the whole lattice is relaxed until no two neighbouring corners
  differ by more than a car can climb, so the excess spreads outward until there
  is room for it.
- **Carving is done in two passes, not one.** With each sample stamping as it
  goes, a later sample's verge lands on an earlier sample's road and the route
  gets bitten into wherever it passes near itself. The first pass records the
  nearest route point per corner; the second applies it. The result does not
  depend on which end the route was walked from.
- **A named track that would not load used to race anyway.** The failure was
  logged and then the "a track was named" branch skipped the fallback, leaving
  the track all zeros - no tiles wide - and sampling it indexed off the front of
  its own arrays. Found only because a test hard-coded a generated track's name
  and the generator had renamed it.

**What moved, deliberately.** The generator hash in `src/frontend/cli/golden.h`,
because every seed now builds a different track; and the track file format, from
version 2 to version 3, to carry the route kind. **Version 2 files still load**,
as sprints, which is what every one of them was - the test pins the old shared
code and requires it still to decode. The **world hash did not move**: the
physics is untouched by any of this.

All 22 shipped tracks are regenerated. Generated ones now run 6 to 10 gates over
52x52 to 64x32 rather than 2 gates over 36x18; `the-oval`, `the-crossing` and
`the-long-way-round` are marked as the circuits they always were.
`gearstick_cli generate 200` reports every one driveable.

### A parts box, after the original's

**"Let us make the track editor more like the original editor, where there were
different blocks that could be dropped onto the map and then modified for height
/ slope / width / shape — and we need blocks for start line, finish line or
combined start / finish line for circuits."**

The editor had brushes: raise a corner, paint a tile, place a gate. Brushes are
how you shape *ground*, and they are not how you build a *road* - laying a
straight by hand means keeping forty corners level yourself, and the moment you
stop being perfect the car is tipped sideways by its own road. The original had
a PARTS BOX beside the course for exactly this reason.

**A part is a way of editing, not a second track format.** Everything a piece
does is corner moves, surface changes and gate placements - the edits that
already existed - grouped into one transaction. So a piece undoes in one step, a
track built from parts is byte-for-byte the same kind of file as one built with
brushes, and nothing downstream needs to know parts happened.

Nine pieces, in `src/core/gs_parts.h`:

- **Road**: straight, corner, ramp, crest, dip. Each lays level ground across
  its width and its own surface, following the ground along its length - the
  same rule the generator carves by, and for the same reason.
- **Route**: start line, finish line, **combined start / finish**, checkpoint.

Each carries what can be modified about it - turn in quarters, width, length,
rise, what it is made of - and choosing a piece loads numbers worth dropping,
because a corner's length is a radius and a straight's is a distance and
carrying one into the other gives a shape nobody asked for.

**The three lines are three different things, and dropping one says what kind of
track this is.** A combined line makes it a circuit; a separate start or finish
makes it a path. That is `gs_edit_route_kind`, which is undoable like everything
else, because what a track *is* belongs in the history.

**A start line becomes gate zero wherever it was dropped.** Gate zero is where a
race begins, so a start line placed after the corners have been laid has to move
to the front of the route - otherwise building a track in the order the pieces
occur to somebody gives a track that starts in the middle of itself. That needed
`gs_edit_move_gate`, also undoable.

The hover preview shows the piece's footprint before the button goes down, in
red when it will not fit: a brush affects the tile under the pointer and a part
affects forty of them, and the piece that will not fit is exactly the one whose
edges you cannot see.

Five tests: a part undoes in one step and redoes to the same track; a road piece
is level across its width and lays its surface only where the road is; a start
line dropped last is still gate zero and undoes cleanly; a combined line makes
the track a loop and taking it back makes it a path again; and a part that will
not fit changes nothing at all.

**What is not here: intersections and overpasses.** Asked for, and not
representable in the current data model - terrain is a single per-corner
heightfield, one height per point, so two roads cannot occupy the same tile at
different heights. It needs a separate bridge layer in the track and a "which
level am I on" bit per car in the simulation, which is a real design decision
rather than another part in the box. A *flat* crossroads is representable and is
the obvious next piece.

### A finish line that fires, and a lap that means a lap

**"I drove across the finish line and the game did not recognise it."**

Three faults, all in the same handful of lines, and the first is the one that
bit.

**A gate is finite across its line** - that is what makes it a gate rather than
a tripwire across the world - and the generator was laying gates three and four
tiles either side of a road it had just carved four tiles either side of. So a
car keeping to the outside of its own road went *past* a checkpoint without
crossing it. Gates count in order, so `next_gate` never advanced, and the finish
line then did nothing at all when it was reached.

**Why nothing caught it.** `gs_analyse` races the AI, and the AI aims at gate
centres - so the AI never missed a gate and every track reported completable. It
is the same shape of mistake as the one before it: the check measured something
true and adjacent to the thing that mattered.
`every_gate_is_wider_than_the_road_it_crosses` now states the rule directly, and
`GS_GEN_ROAD` moved into `gs_generate.h` so a test can say it.

**A lap of a loop was one short.** With gate zero as a circuit's finish gate,
the crossing a car makes on its way *out* of the grid counted as a lap - a car
starts behind the start line, so it crosses it once before racing. A three-lap
race ended after two. `gs_car_laps_done` asks the route rather than reading
`laps`, and the HUD asks it too.

**And a path was raced for three laps.** A sprint has a start at one end and a
finish at the other; "three laps" of one means driving back down it twice with
nothing marking the way. `gs_world_laps_needed` returns the chosen number for a
loop and one for a path, because arriving is the whole race.

### A tree with three colours, and ten seconds to read the road

**"The start count for a race should be longer, ten seconds - we should actually
show a real racing tree with red, orange, green lights."**

Three seconds was not enough to settle your hands, look at where the road goes
and pick a gear; a race that starts before you have read the first corner starts
without you. Ten now. And counting lamps down one a second told you how long was
left and nothing about what to *do* with it - red, amber and green are read
without being counted, and every driver already knows them. Red for the wait,
with the lamps falling away as it shortens; amber for the last three seconds;
green on the tick the simulation stops holding the cars.

### Escape backs out of a race instead of restarting it

**"When I press Escape in the track, it no longer takes me to the menu - it
restarts the race."** Exactly what it did. Escape out of an online race goes to
the lobby, and the lobby starts a race the moment it is ready - so on a lobby
that was already full, leaving put the player straight back into the race they
had just left, with no way to a menu at all.

The lobby waits now, once a player has left a race under their own steam, and
offers **Race** to go again. Auto-start is right the first time and wrong every
time after.

### A crossroads in the parts box

Two roads meeting, level, both ways open - laid as a plus rather than a square
so the corners of the junction are ground rather than road. **Flat, and only
flat.** Two roads at *different* heights is an overpass, and that is not
something this terrain can hold: it is one height per corner and an overpass
needs two. It stays a design decision rather than another piece in the box.

### An online race that could be finished at all

**"I drove through the finish line and it still didn't recognise it."** Reported
twice, and the second time the cause was different from the first.

**Online races never had a lap count.** `gs_start_race` has two branches: one
builds the grid from the server and one from the setup screen. The second sets
the mode and the lap count; the first sets **neither**, so `laps_to_win` stayed
at the zero `gs_world_init` leaves - and zero means *a race with no finish line
at all*, which is what a test drive is. The finish block was skipped entirely.
Every online race was unfinishable, and had been since online racing existed.

It is not read off this machine's setup screen, for the same reason the grid is
not: an online race is the server's race, and two machines reading their own
screens build two different worlds. The protocol carries no lap count, so it is
derived from the track - which every machine has and has had checked on the way
in. `GS_DEFAULT_LAPS` is three, the number the original's Grand Prix circuits
were raced over.

**And a path is raced once.** Asked for directly and it is the only thing that
makes sense: a path has a start at one end and a finish at the other, so "three
laps" of one means driving back down it twice with nothing marking the way. The
setup screen no longer offers the slider for a path, because a control that is
quietly ignored is worse than one that is not offered.

### The check that was missing, twice

`gs_analyse` reported a track "completable" if `laps > 0` after an AI drive. On
a circuit, `laps` counts crossings of gate zero - and the car crosses gate zero
**leaving the grid**, a few car lengths into the race. So the check was
answering "did the car manage to drive over the start line", and every track
passed however impossible the rest of it was. That green tick is what carried
two rounds of unraceable tracks.

It asks `gs_car_laps_done` now - a whole lap of a loop, or the arrival at the
end of a path. And `a_generated_race_can_actually_be_finished` races the AI on
twelve seeds and requires `finish_tick`: what a player does is *finish*, so that
is what is checked.

### The light tree stood off the side of the screen

Placed a fixed distance outside the *gate's* edge - so when gates were widened
to span the road properly, the tree went three tiles out with them and left the
frame at the zoom a race is actually driven at. A player looked for it and it
was not there. It is tied to the road now, which is what it stands beside and
what does not change when a gate's width does, and pulled in if the gate is
narrower than that.

### The construction set, where somebody would look for it

**"The way you get to a track editor is stupid. We should be able to create /
delete a new track from the tracks menu, as well as being able to edit a track.
Hitting Tab off the main menu to edit a track is stupid and not obvious."**

It was. Tab from anywhere opened the construction set, and the one screen that
is *about* tracks never mentioned it existed. The tracks screen now has **New**,
**Edit** and **Delete** beside Load, and Tab still works for anybody who learned
it.

The menu does none of it. It cannot open the construction set, it cannot talk to
a server, and it should not learn how - so it raises a request and the frontend
acts on it, the same way the race setup is handed over rather than acted on
there.

- **New** starts a blank 48x40 flat field rather than whatever happened to be
  loaded, because levelling somebody else's hills is not the start of an idea.
- **Edit** opens the picked track. On a track that came with the game the button
  says **Edit a copy**, and that is what it does.

### A track that came with the game is not yours to change

`gs_library_entry.builtin` says where an entry came from. It is not a property
of the track - the same ground built by hand is an ordinary track - which is why
it sits beside the name rather than anywhere near the content hash.

A shipped track cannot be renamed or deleted, and editing one puts a copy in the
library and edits that, so the library a player came with is still there after an
afternoon of building. The flag survives the round trip, or the protection would
last until the game was next started and then quietly stop.

The library file went to version 2 to carry it. **Version 1 still loads**, with
every entry treated as the player's own - which is what they all were, before the
game shipped a library of its own.

### Handing a track to somebody

The store had all of this already - `GS_TRACK_PRIVATE`, `GS_TRACK_SHARED`,
`GS_TRACK_PUBLIC`, per-key sharing, and shipped tracks outside all of it - and
so did the wire, in `gs_wire_publish`, `gs_wire_withdraw` and `gs_wire_share`.
What was missing was any way to ask for it.

Under the picked track there is now:

- **The code**, always, because a track as text needs nobody's permission. Ready
  to copy, and rebuilt only when the selection changes rather than sixty times a
  second for a field nobody may be looking at.
- **Publish to everybody** and **Take it down**, where there is a server.
- **One button per person in the lobby**, to hand it over or take it back. They
  are named by the public key the server watched them prove, not by a string
  somebody typed - which is the whole reason sharing is with people you are
  actually in a room with.

**The detail block scrolls in a box of its own.** It grew a name, a note, a
code, publishing and a row per person you could hand it to - and the last of
those has no fixed size, because it depends on who is in the room. A panel whose
height depends on that is a panel that is the right size until somebody joins.
The box is only there when there is a track to detail, so browsing gets twelve
rows of library rather than five.

`no_screen_is_drawn_bigger_than_the_window_it_is_in` caught the overflow, but
only after the fixture was made to pick a track and go online: it built a full
library and selected nothing, so the screen was measured at its smallest -
which is exactly what that test's own comment warns against.

### A Race button that did nothing

**"I get to the lobby and click Race and nothing happens. Surely you could have
tested for such an obvious bug."** Correct on both counts.

The button asked `count >= capacity`. Before the server has answered, a lobby
has neither - both are zero - so `0 >= 0` offered **Race** to somebody still
knocking on the door, and pressing it did nothing because `gs_wire_ready` was
false. It also read through the lobby pointer without checking it was there.
Every other line on that screen already goes through `heard` or `lobby_ready`
for exactly this reason; the new control did not.

The condition is `gs_menu_lobby_can_race` now - a predicate rather than an
expression buried in the drawing, **because a predicate is a thing a test can
call**. That is the actual failure here: not the wrong comparison, but putting
it somewhere nothing could reach.
`the_lobby_offers_a_race_only_when_it_could_start_one` walks every state the
screen has - no lobby, knocking, waiting on somebody, receiving the track,
ready, and a one-player lobby - and the version that shipped fails two of them.

### Getting around, and a box of parts you can see

**"When editing, I don't have the toolbox to select from, and I don't have the
ability to scroll around the track."** Both true.

**Panning was the arrow keys and nothing else**, on a board up to sixty-four
tiles across at a zoom that shows a dozen - so getting anywhere meant holding a
key and waiting, with one hand already on the mouse. Dragging with the right or
middle button moves the board under the pointer, and the wheel zooms. The
isometric axes are the screen's diagonals, so a drag in pixels is undone by
moving the camera along both world axes at once - the inverse of
`gs_iso_project`, with the zoom taken out.

**And the parts box is a window of its own, in the top right.** It was a combo
inside the brush palette, which is a mode inside a mode: choose the parts brush,
then open a menu, then read nine words. The original put the PARTS BOX beside
the COURSE and that is the whole shape of the thing - what you are building on
over here, what you can build with over there. Nine pieces as buttons, the
chosen one lit, and clicking one selects the parts tool as well, because a
palette that does not select anything feels broken.

Its sliders reserve the widest label, the same rule the brush palette follows -
ImGui clips a label that does not fit, which is how "turn (quarters)" came to
read "turn (quarter".

### Tab is no longer the front door

**"Remove the tab to edit."** Tab opened the construction set from anywhere,
which was how somebody was expected to find it. New and Edit on the tracks
screen are how you get in now.

Tab still works *inside* a building session, because it is also the
build-drive-build loop - the single biggest thing the original could not do -
and losing that to fix a discoverability problem would be a poor trade. It is
armed when the tracks screen opens the editor and disarmed on the way back to a
menu, so it is the loop while you are working on something and nothing at all
once you are not.

### A window with its own icon

**"We need a gearstick icon in the caption base of the window."** An untitled
window wearing the toolkit's default icon is what an unfinished thing looks
like, and it is the first thing anybody sees of the game.

`assets/icon.png` is a gear lever - a red knob on a shaft out of a gaiter, over
the shift pattern's gate. At sixteen pixels almost none of that survives; what
survives is a round red knob above a pale diagonal, which is what the shape is
chosen to keep.

**Generated, not drawn**, like the vehicles and the trig tables:
`tools/make_icon.py` draws it from shapes described in the script and writes the
PNG with the Python standard library alone, so it can be regenerated on any
machine rather than only one with an imaging library. It is the one image file
in the game and `assets/ATTRIBUTION.md` says so.

**And an image library, which is a new dependency.** `ext/sdl_image` at
`release-3.4.4`, configured down to PNG and nothing else - nineteen other
formats off, every save path off. It is linked by the shell only; `src/core/`
still links nothing and `gearstick_cli` still lists libc and the loader.

PNG goes through the system libpng, which was asked for by name. That is worth
recording because it hit exactly the trap `CLAUDE.md` had already written down:
the configure failed on `ZLIB::zlibstatic` referencing a `/usr/lib64/libz.a`
that the system zlib package does not ship, in an error naming neither zlib nor
SDL_image. The missing piece on this machine was `zlib-ng-compat-static`.
`SDLIMAGE_PNG_LIBPNG OFF` remains the one-line way back to the built-in stb
decoder, which needs no system library at all.

### The results screen you could not leave

**"I completed a race on one track, then went to the tracks menu, selected
another and then race, and I saw this screen"** - the results, again.

The end-of-race path is guarded by `race_settled`, worked out once so the same
lap is not resubmitted every frame. Going back to the lobby cleared that flag -
**and the world it describes is the finished one**, still loaded until a new
race replaces it. So the next frame found `world.over` true and the flag clear,
ran the whole ending a second time, submitted the same result again and put the
screen back on the results. The client log shows it plainly: *the race is agreed
at tick 2619, and submitted*, then *at tick 2620, and submitted*.

It is cleared in `gs_start_race` and nowhere else now, which is the only moment
it is honestly clear: when there is a new race for it to describe.

### An online flow with no dead ends in it

The same report exposed a worse thing behind it. **Racing is not this machine's
to start while it is on a server** - the track is the server's, the grid is the
server's, and so is the moment it begins. But the tracks screen offered *Race
this one*, which went to the local setup screen, which cannot start a server
race; and the results screen offered *Setup*, which goes to the same place.
Reading that screen at all would build a different world from everybody else's,
which is the one thing rollback cannot recover from.

Online, the tracks screen loads a track to look at, edit or share and says so;
the results screen's big button says **Back to the lobby**, because that is
where a race is decided and what pressing it actually does; and *Setup* is
offered offline only.

### A door nobody answers

**"I still can't start a race - I keep seeing this screen."** The lobby, on
"Knocking...", forever.

A server that *refuses* sends a reason and it arrives as `lobby_error`. A server
that cannot decrypt what we sent has nothing to reply to at all - which is what
a wrong key looks like - so the screen said "Knocking..." for as long as anybody
was willing to watch it. Both a slow handshake and a hopeless one looked
identical.

After `GS_KNOCK_PATIENCE` seconds with no answer it now says so, and names the
three things it is: the server is not running, the address is wrong, or the key
is not the one the server prints when it starts. `gs_menu_lobby_unanswered` is a
predicate for the same reason `gs_menu_lobby_can_race` is - it is a thing a test
can call.

In this case it was the third. The test harness had been handing the client a
key from a server whose store had since been deleted, so it had minted a new
one; and an hour-old server process was still holding the port, so the
replacement never bound.

---

## Walking the front end by machine, 2026-08-23

**What is true today: the front end is walked one move deep, and that walk
passes.** It is not coverage of the paths through the front end, the editor is
not in it at all, and neither is to be described otherwise. Phase 17 of the
completion plan is the work to make it one; this section is the detail behind
those items.

### The crash that stopped it

Two sessions died on 2026-08-23 with the terminal falling straight back to
PowerShell. It looked like WSL breaking and it was not. `gearstick_render_tests`
allocated at about 37 MB a second without bound; the WSL VM here gets 15.5 GB,
and when it runs out the whole VM goes rather than the process — no Linux OOM
message, no Windows bugcheck, and `journalctl` is volatile-only here so the
previous boot leaves nothing to read. Both deaths were within minutes of
starting that binary — one from a rebuild-and-run, one from `ctest -R
gearstick_render`.

Reproduced under a 3 GB cgroup cap it reached 2.5 GB still climbing, inside
`every_screen_has_a_way_off_it_and_the_ways_lead_somewhere_real`. ASan's live
memory profile named the holder exactly: 805 MB in **two** allocations under
`SDL_AllocateRenderVertices`, and 391 MB in **4.4 million** allocations under
`AllocateRenderCommand`, both below
`ImGui_ImplSDLRenderer3_RenderDrawData`. That is SDL's render command queue.
`FlushRenderCommands` (`ext/sdl/src/render/SDL_render.c:310`) is what recycles
the commands and resets `vertex_data_used`, and it runs on present or on an
explicit flush. `gs_ui_frame` drew and never presented, so all forty-odd
thousand frames of the walk stayed queued. The other tests in that file are
presentless too and get away with it because `SDL_RenderReadPixels` flushes;
this one reads no pixels.

The fix is one line in `gs_ui_frame` — `SDL_RenderPresent(ui->ren)`, which is
what the game itself does at `src/frontend/game/main.c:2057`. After it: memory
flat at about 455 MB, peak 557 MB, every renderer test passing, 7:40 wall clock.
`gs_sanitizer_env` in `cmake/CompilerWarnings.cmake` now also sets
`ASAN_OPTIONS=hard_rss_limit_mb=2048`, so the next runaway of this kind stops
itself and says so instead of ending the session.

**Still red on time, not on memory:** `ctest -R gearstick_render` reports
`***Timeout 180.11 sec`. The suite needs 7:40 and the test property allows 180.

### What the walk covers, exactly

`gs_ui_exits` takes each of the eight screens in `gs_every_screen` — every
`gs_screen` except `GS_SCREEN_RACE`, correctly, since that is not a panel — and
for *n* from 0 to 71 starts from a **fresh** menu on that screen, presses Tab
*n* times, presses Space, and records the destination if the screen changed.
Two things are then asserted and only two: `CHECK(found > 0)` for every screen
but `GS_SCREEN_LOGIN` and `GS_SCREEN_TRACKS`, and a breadth-first `CHECK(home)`
that the title is reachable, with the same two exempt.

The third property its own comment claims — that no exit lands on a screen that
is not a screen — **is now asserted**, and was not before: `m.screen <
GS_SCREEN_COUNT` appeared in `gs_ui_exits` only as a filter, so an out-of-range
destination was dropped in silence by the same condition that filtered out a
screen leading to itself. A `CHECK` now runs before that filter. Verified by
injection rather than by reading it: one control in `gs_menu.c` rigged to return
`GS_SCREEN_COUNT + 1` turns the walk red and names it, and restoring the control
turns it green.

What it therefore does not cover: anything two presses deep, because the menu is
reset every iteration on purpose; anything reachable only from a different
starting state, and there are six `ImGui_BeginDisabled` sites in `gs_menu.c`
whose controls are dead in the one state it uses; anything needing Escape, the
arrow keys, the mouse or typed text; any control past the seventy-second Tab;
any control whose fault does not change the screen; every player count except
the one the fresh menu holds; and the editor, which is not a `gs_screen`.

### Notes for the exhaustive walk

- **Nothing in this walk needs pixels, and it no longer asks for any.** Done:
  `gs_ui_frame` now runs the two backend `NewFrame` calls, `ImGui_NewFrame`,
  `gs_menu_frame` and `ImGui_Render`, and stops there. `SDL_RenderClear`,
  `cImGui_ImplSDLRenderer3_RenderDrawData` and the `SDL_RenderPresent` that had
  been added to stop the queue growing are all gone — with nothing drawn there
  is no queue to grow. `ImGui_Render` stays because ending the frame is what
  settles focus for the next one; it builds draw data in memory that nobody
  reads. **The renderer suite went from 7:40 to 12.37s**, every test still
  passing and saying the same thing, which is a 37x step and the difference
  between "a sample is all we can afford" and "walk the whole thing". The tests
  that do want pixels use `gs_panel_of` and a frame of their own, untouched.
- **The cost model, which is what made a sample look sensible.** Per screen the walk draws
  `72 x 4 + 2 x (71 x 72 / 2)` = 5,400 frames, so 43,200 across eight screens,
  at roughly 8 ms each — a software-rendered 1280x720 ImGui frame under ASan.
  Reaching the *n*th control by pressing Tab *n* times is the quadratic term and
  it is also what makes the map say "the fifth control" instead of naming a
  button. Addressing controls by their ImGui ID fixes the cost and the staleness
  together. `imgui_internal.h` is where nav focus can be set directly; it is
  vendored and pinned, so using it is a decision to take rather than a risk to
  carry.
- **Controls are enumerated and pressed by name. Done.** `src/ui/gs_ui_probe`
  implements the four hooks Dear ImGui's own test engine registers —
  `ImGuiTestEngineHook_ItemAdd`, `_ItemInfo`, `_Log` and
  `ImGuiTestEngine_FindItemDebugLabel` — and nothing else: no test engine, no
  new submodule, about 120 lines of C++ behind a C header. `IMGUI_ENABLE_TEST_ENGINE`
  is defined for the whole `gearstick_ui` target, which compiles in call sites
  of the form `if (g.TestEngineHookItems)` against a flag that is false unless a
  test sets it, so the game pays one predictable branch per widget. The define
  does not appear in any `#ifdef` inside `ImGuiContext`, so there is no ABI
  question; it gates declarations and macros only.

  What comes back per item is the ImGui id, the label, the owning window, and
  whether it was disabled or unreachable by nav. **The id is the identity** — a
  hash of how the control was made and of the id stack it was made in, not of
  where it sits — which is what makes a map keyed on it survive an edit.
  Measured, not assumed: a `ImGui_Button` inserted above everything else on the
  title screen added two entries to the map (itself and one structural item) and
  left all 190 pre-existing controls on the same ids.

  `ImGui::ActivateItemByID` presses one directly, which costs one frame instead
  of the *n* frames of tabbing to the nth control — the second half of what made
  exhaustive look unaffordable. It also presses what the keyboard cannot reach.

  ImGui names the widgets a person presses and leaves its own structural items
  anonymous — table cells, child regions, groups — so the test asserts a name on
  the ones that matter: **a control that moves a player between screens must be
  named.** Verified by injection: making the label hook drop its labels turns
  the walk red, restoring it turns it green. The probe is first-party, so it
  carries the project's warning set by hand rather than going through
  `gs_configure()` — that set has `-Wstrict-prototypes` and
  `-Wmissing-prototypes` in it, which GCC rejects for C++ and `-Werror` then
  makes fatal. ImGui's own headers are included inside a `#pragma GCC
  diagnostic push/ignored` for the conversion warnings, which is narrower than
  exempting the file and keeps our own code strict.
- **Recognising a state already seen. Done: `gs_menu_hash`.** The plan had said
  `gs_menu` was a plain value with one borrowed pointer in it. It has **two** —
  `const gs_lobby *lobby` and `const char *lobby_error` — and three more fields
  that have no business in a state hash, which is what the work turned up:

  - `lobby` is skipped. It is a view of the frontend's state, not the menu's:
    two menus pointed at the same lobby are in the same state, and a different
    lobby is something to seed a walk with rather than something a press finds.
  - `lobby_error` is hashed **by its message, with its terminator, and not by
    its pointer**. The text is what a player reads and is therefore state; the
    address it happens to sit at is not, and hashing that would make one message
    read as two states.
  - `track_progress` and `knocking_for` are skipped because they advance with
    the clock. Hashing a clock makes every frame a state nobody has been in,
    which is exactly as useful as no hash at all.
  - `panel` is skipped: it is a measurement written *by* drawing, an output
    rather than an input, and it moves when the window is resized.

  Everything else is in by byte range rather than field by field, so a field
  added to `gs_menu` is hashed the day it is added rather than the day somebody
  remembers to add it. FNV-1a; nothing is stored under this hash and nothing
  travels, so the reasons to reach for something stronger do not apply.

  **All 52 fields are classified in the test and each is proved by flipping a
  byte in it** — the 48 that are state have to move the number, the four that
  are not have to leave it alone. Both directions are verified by injection:
  truncating the hash's first range prints `NOT IN THE STATE: chosen` and the
  seven fields after it, and hashing the whole struct instead prints `IN THE
  STATE AND SHOULD NOT BE: lobby, track_progress, knocking_for, panel`.

  One thing the work turned up that is worth keeping: **two rosters built from
  scratch do not hash alike**, because making a driver mints a random salt for
  their password. That is correct rather than a fault, and it is the proof the
  hash reaches deeper than the fields a screen draws. The property a walk needs
  is the weaker one, and it is the one asserted: *a copy of a state is that
  state*, which also pins that struct assignment carries the padding the hash
  reads.
- **The player count is a dimension, not a value.** One to four players is one
  to four grid rows, each carrying a driver, a vehicle and a colour, so the set
  of controls differs at each count rather than merely growing.
- **The editor is where the combinations live, and they are all walked.** The
  standard for this phase is every scenario, not a sample — see the Discipline
  section of `CLAUDE.md`. An edit is simulation work on a plain value with no
  drawing in it, so once the walk stops rasterising, a combination costs
  microseconds and there is nothing to trade away. Anything that genuinely
  cannot be reached is named in the test with its reason, and the walk asserts
  that what it covered equals what exists rather than leaving the reader to
  assume it.

### The walk, going as deep as the front end does

**Done, and the number is 694 of 694.** Every control the front end offers in a
state where it can be pressed, is pressed — asserted by the test, not reported
by it. Eleven more are counted separately as never pressable: drawn dead in
every state they appear in, or items nav can never land on.

Getting there needed two corrections that are the substance of this item.

**Walking the menu's bytes does not terminate and cannot.** The front end offers
32 tracks against 8 vehicles against 16 colours against 4 player slots, so the
values a `gs_menu` can hold run to the millions; a walk over them covers a
vanishing fraction however long it is left. Measured rather than assumed: keyed
on `gs_menu_hash`, the title screen alone was still going after nine and a half
minutes and roughly 1.8 million presses. So a state is now **what the front end
is showing and what it will let you press** — the screen, every control on it,
and whether each is live or dead. Two menus offering the same controls in the
same conditions are the same place to be standing, whichever of the 32 tracks
happens to be highlighted. That space is finite, and it came to 102 offerings.

**Novelty on its own does not converge either.** With offerings as the key, the
walk found `track number 13 → Delete → track number 14 → Delete → …`, emptying
the library one track at a time: every shorter list is a genuinely new offering,
and all of it is one Delete button doing one thing. So the reason to queue a
state is no longer that it is new but that **it offers a control nobody has
pressed yet**. That converges by construction — each visit presses something for
the first time, and the controls are finite — and it turns the exhaustive claim
into something a `CHECK` can hold.

Two smaller things, both measured:

- **A state is stood in by replaying the path to it, and that is not the same as
  copying the menu back.** The first attempt did the cheap thing and the
  determinism check caught it: a menu is the whole of its own state but not the
  whole of the state on screen — ImGui's focus, active item and open popups live
  outside it — so the same control pressed after a different number of frames
  does a different thing. Replay is now the only way to stand anywhere, and
  `gs_ui_probe_settle` puts ImGui back to a standing start between presses.
- **The seeds share one set of books.** From any screen you can reach the
  others, so eight seeds with eight sets of notes covered the same graph eight
  times: 27,702 presses to learn what 4,111 now learn. Seeds 2, 6, 7 and 8 add
  nothing at all, which is itself the proof the front end is connected; login
  adds its own 13 because it is a closed door until the walk can type.

The hash was 87% of the cost at 1.18 ms a call — FNV a byte at a time over a
617 KB menu, twice per edge. Eight bytes at a time took it to 0.18 ms and a
press to 0.32 ms all in. It reads words in whatever order the machine stores
them, which would matter if this number ever travelled or was written down; it
does neither.

**Left for the items after this one:** 1,424 presses changed nothing at all.
That is expected for a control pressed in a state where it has nothing to do,
and it is *not* yet the check that no control does nothing in every state it
appears in — that is a separate plan item and this counts what it will need.

### Every way in: the walk opens the door itself

**Done.** The walk can press Escape and the arrows, and it can type. Seeded
signed out at the login screen with nothing filled in and no screen handed to
it, it reaches the title in two moves: `44 states, 237 actions (75 typed), 2
deep, 13 of 13 pressed, 0 capped`.

**The door wants a name and a password typed, not a driver picked.** That is not
what the walk was built expecting, and it is the sort of thing only a walk that
has to open a door for itself turns up: the first attempts failed with
`pick=-1` and *"that name and password do not match"*, because the vocabulary
held the password and not the name. A walk can only try what it has been told
exists — the words it knows are the driver's name, the right password and a
wrong one, and that last one is there so that getting in is not something that
happens to anything typed at it.

Three things had to change and each was a wrong idea failing loudly:

- **Coverage cannot be the reason to explore.** Requiring that a state offer a
  control nobody had pressed converges beautifully and stops one press outside
  the door: signing in is three controls, all of which have been pressed by the
  time the sequence matters. Novelty drives the walk; coverage is what it
  reports.
- **The offering alone cannot see a form being filled in.** The login screen
  draws the same controls whatever has been typed, so keying states on what is
  on offer makes the sign-in sequence unreachable *in principle*. There are now
  two keys for two questions: the coverage walk keys on the offering, and the
  door walk keys on the offering plus the scratch fields — `profiles`,
  `records` and `library` left out of both, being all of the combinatorics and
  none of the navigation.
- **Restoring a menu is standing in a state — the first attempt at it was
  snapshotted a frame late.** Replaying a path costs its depth on every action,
  so the obvious saving is one memcpy. That was tried, the assertion that it
  lands in the state it left failed on every screen, and the conclusion drawn
  here was that a menu is not the whole of the state on screen. **That
  conclusion was wrong.** The snapshot was being taken after the enumeration
  frame, which is one frame further on than the path describes, so it could
  never have matched. Taken immediately after the replay it matches every time.

  Restoring is now what the walk does, with `gs_menu_hash(&m) == here.hash`
  asserted on **every action** rather than assumed - so the soundness of it is
  checked roughly 12,700 times a run instead of argued about. The renderer suite
  went from 110 s to 30 s for identical coverage: 727 of 727 controls, 12,714
  actions. ImGui is still put back to a standing start between actions with
  `gs_ui_probe_settle`, which is the part of the original reasoning that held.

Cost: the renderer suite is 30 s. The coverage walk is
`6,175 states, 12,714 actions, 727 of 727 pressable controls pressed, 11 never
pressable`, with 5,915 states declined because their offering had already been
entered.

**The weakness in that number, stated plainly.** `727 of 727` is 100% of what
this walk saw. A richer alphabet saw 758 and also said 100%. The denominator is
self-referential, so `CHECK(pressed == offered)` is necessary and nowhere near
sufficient — it is joined by `CHECK(n_offered >= 727)`, pinned the way the
golden replay is pinned: raising it is the front end growing, lowering it is a
deliberate act that wants a note here saying which controls stopped being
reachable. The real fix is a count taken without asking a walk what it found,
and that is the last item of Phase 17.

### The pipeline, red since the icon landed

Three failures, none of them in the front-end work that exposed them, and all
three the same shape: **a change that was green on one machine and had never
been near CI.** Thirteen commits sat unpushed; the first push ran the pipeline
against all of them at once.

- **`ext/sdl_image` was in `.gitmodules` and in none of the lists that check
  submodules out.** Four lists across `ci.yml` and `package.yml`, plus the
  README's clone line - so following the README on a fresh clone failed at
  configure with `ext/sdl_image is empty`, exactly as the runner did. That list
  is the thing that keeps the README honest, and it was the thing that was
  wrong.
- **SDL_image was decoding PNG through libpng.** On the RHEL job that fails
  configure: libpng is found by name, libpng needs zlib, and RHEL's zlib ships a
  CMake config advertising `/usr/lib64/libz.a` which is not in the package. The
  error names neither libpng nor zlib nor gearstick - it is `find_package(PNG)`
  four frames down inside `ext/sdl_image`. **The note above that setting had
  predicted this failure and named its one-line fix**, `SDLIMAGE_PNG_LIBPNG
  OFF`, which is the only reason it cost minutes. PNG now goes through the stb
  decoder that comes with SDL_image and needs no system library at all.
- **The shipped library was stale.** `assets/server/gearstick.db` is a
  generated artefact and CI rebuilds it to check that it is what the tools
  produce. It was not: `51ff86e` took `GS_LIBRARY_VERSION` to 2 and added a
  `builtin` flag, and the database was never rebuilt after it. Reproduced
  against CI's own build rather than the local one - the pinned SQLite
  amalgamation, not the system's, because identical rows laid out by two
  versions are two different files. The generators are deterministic: two runs
  byte-identical, so the diff was staleness and not flakiness.

The renderer suite's ctest budget goes from three minutes to ten in the same
change. It was three from when the heaviest thing in it photographed a frame; it
now walks the whole front end under sanitizers, 111 s here, and the runners are
slower than this machine.

### The conditions the buttons are under

**Done, and the result is small enough to be worth stating carefully.** The walk
starts from seven menus instead of one, sharing a single set of books so that
each seed is only ever asked what it reaches that the ones before it did not:

| seed | new states | new controls |
|---|---|---|
| everything | 6,179 | 727 |
| signed out | 6 | 0 |
| **offline** | **4,741** | **3** |
| an empty library | 56 | 0 |
| no track picked | 88 | 0 |
| no results yet | 101 | 0 |
| alone in the lobby | 135 | 0 |
| one player | 94 | 0 |
| two players | 101 | 0 |
| **three players** | 108 | **10** |
| **four players** | 115 | **10** |
| a guest racing | 101 | 0 |

**750 of 750** pressable controls pressed, 13 never pressable, 26,290 actions,
57 s - and **23 of the 750 are reachable only by seeding**, up from 3.

**The player count turned out to be the second thing no control can reach.**
Three players draws ten controls no other starting state does and four players
draws ten more, which says the setup screen at four is a *different screen* from
the one at two rather than the same screen with more rows - each grid row
carries its own driver, vehicle and colour. The walk can change a lot by
pressing, but it cannot change how many people are playing, any more than it can
decide this copy was pointed at a server.

The floor moved from 727 to 750 with that change, deliberately and with the
reason written beside it. Raising a floor because the front end genuinely grew
is what it is for; raising it because the last run happened to measure more is
how a tripwire quietly becomes a ratchet.

**Six of the seven seeds add no controls at all, and that is a good result
rather than a wasted one.** It says the walk can press its way into most of
these conditions by itself: it can sign out, it can deselect a track, it can
empty a library one Delete at a time. The seed that pays is *offline* - and
offline is precisely the condition **no button can change**, because whether
this copy of the game was pointed at a server is decided before the front end
draws anything. That is the shape of what seeding is for, and it was not obvious
before the numbers came back.

Two things the run also settles:

- **Six controls are drawn dead in one state and pressed in another.** That is
  the conditional half of the front end being exercised rather than skipped.
- **3 of the 730 are reachable only by seeding**, and that is asserted rather
  than printed: `CHECK(w.n_offered > alone)`. If it ever comes back zero, either
  the seeds have stopped differing or the front end has stopped putting
  conditions on its buttons, and both want looking at rather than passing
  quietly.

The floor was pinned at 727 rather than raised to 730 at this point - a floor is
there to catch the walk seeing *less*, and pinning it to the last measurement
each time turns a tripwire into a ratchet that reports whatever it just did. It
has moved four times since, each time deliberately and each time with the reason
beside it in the test: to 750 when seeding at every player count found twenty
more, down to 663 when the walk stopped counting rows it could not press, and up
to 765 as scrolling, the sweep inside combo boxes and four more starting states
reached what was left.

### The construction set, walked and measured

**Nothing had ever pressed a button in the editor.** `gs_editor_frame` was
called in one place in the whole repository and that place was `main.c`; the
editor tests that exist drive `gs_editor_paint` and set `e->brush` by hand,
which measures the brush engine and says nothing about the palette a player
chooses a brush with. Both halves are now covered, and they are covered by
different tests because they are different claims.

**What the tool does: 5,563 option values, every one of them.** Against the
panel's own ranges - radius 0 to 8, step 0.05 to 2, gravity 0 to 3.9, gate
heading 0 to 359, half width 0.5 to 8, every surface, every piece in the parts
box, and the four dials. The continuous ones are walked at a hundredth, which is
finer than the panel displays and finer than a mouse can land on. Four things
that came out of writing it:

- **Heights are kept in 256ths of a tile**, so "the ground moves by exactly the
  step" is only exactly true for steps that land on that grid. The check is to
  the storage's own resolution, and the direction is checked separately at every
  step, because those are two different ways for a brush to be wrong.
- **A straight, a corner and a crossroads dropped on flat pavement are a no-op**
  - correctly. They lay level road of their own surface and the ground already
  was that, so the tool says "dropped a straight" and the track is byte for byte
  what it was. The first version of the test called that a bug. Parts are now
  dropped onto ground that has been roughed up and painted something else, and
  what is asserted is that the history gained an entry, because a piece that
  reports itself dropped and records nothing is a piece nobody can undo.
- **The editor's gravity dial goes through `gs_world_init`**, which converts a
  multiple of Earth into an acceleration. Asserting the raw number would have
  passed a version of the bug that once made every race from the setup screen
  run at forty percent of the gravity it claimed.
- **Every gate heading round the full turn** is checked against the conversion
  the editor does, and every half width against the fixed-point it stores.

**What the palette offers: 56 of 56 named controls pressed.** Across 46
configurations - every brush, every surface under the surface brush, every piece
in the parts box, the panels open and shut, and a route with gates on it so the
buttons that remove them are drawn. What is counted is what **moved**: an
activation that lands on the floor is not coverage.

**Searching for the editor's states is the wrong shape, and it took four goes to
see it.** Brush against surface against radius against chosen piece against
which pad slot is being rebound runs to millions; an editor carries a ghost
world and a heat map about with it so only a few hundred snapshots fit in a
queue; and three separate trims of the state key - the track hash, the status
line, the slider values - each changed nothing, because every one of them was
trimming *what varies* rather than changing *why the walk explores*. The rule
that came out of it, and it is the same one the front end's walk taught:

> **A state key holds what changes the offering, and nothing that merely
> changes.** A brush that moves one corner makes a track nobody has seen; the
> status line says "placed gate 3"; a slider nudged by a press is a float nobody
> has seen. None of the three changes what a player can press next.

What decides what the palette shows is a handful of scalars, and they can simply
be set rather than searched for. The sweep does that: 46 configurations, 2,362
actions, 53 seconds, and every named control pressed.

Two more things worth keeping, both about ImGui rather than about gearstick:

- **Activation by name is queued against the navigation window**, and after a
  state is copied back over, focus is often nowhere - so the call is accepted,
  dropped, and reports success. **The frame after a restore is not settled**
  either: ImGui honours an activation aimed at an item it laid out in a
  completed frame, so one frame fails and two succeed. Neither is visible from
  the API and both read as "the editor ignores being pressed".
- **Naming and the keyboard reach different subsets and neither is complete.**
  Pressing by name needs focus parked somewhere; Tab only walks the panel it is
  already in, and the editor keeps three open. The sweep does both - by name
  first, and walked to with the keyboard when that moved nothing.

**Furniture is named, not folded in.** ImGui gives every window a title bar, a
resize grip and an implicit debug window, and they come back looking exactly
like controls - reachable, not disabled, with a name. Counting them would pad
the denominator with somebody else's widgets; silently dropping every unnamed
item instead shrinks it from 76 to 56 and reports a perfect score for less work.
So they are counted apart and printed: 41 unnamed structural items, 253 pieces
of window furniture skipped, and the 56 that are the tool's own controls
asserted.

**The near miss that would have shipped.** Wired to press by id and count the
attempt, this same test reports **50 of 50 controls pressed over a single
state** - a perfect score, having changed nothing whatever. It looks better than
every honest number here. What is counted is now what moved, and the assertions
below it would fail if nothing did:

### The dials the race is set up with

**Every lap count from one to twenty, raced to its finish**, and the crossings
come to one more than the laps every time - the run-up to the line, and the
off-by-one that ends a three-lap race after two.

Getting a car round twenty laps took four harnesses and the first three are
worth knowing about:

- **The racing AI laps a bare rectangle twice and then sits in the run-off.**
  For twenty-five minutes of simulated time, still `active`, lap counter frozen
  at ten. It reads exactly like a bug in lap counting and is a car parked on the
  grass.
- **A generated circuit ends the race inside a minute.** A car dropped anywhere
  but the grid is already off the road; dropped *on* the grid by
  `gs_track_grid` it still finishes with no laps at all.
- **A hand-written shuttle driver leaves the field under either sign of either
  steering rule**, because the conventions were guessed. They were then measured
  instead: heading 0 is +x, the angle increases towards +y, and **RIGHT
  increases the heading while LEFT decreases it**.

What works is a car held on **full lock at a steady speed**. Constant throttle is
a spiral rather than a circle - a faster car turns wider - so the throttle is
held to a speed and the lock never moves, which will lap all day inside a
64-square field. Two gates on that circle, and **the far one faces the way the
car will be going when it gets there**: half a circle later it is travelling in
the opposite direction, and pointed like the first gate the car sails past the
back of it and the lap never completes. That last one cost an hour and looked
like a stuck counter too.

**And the rest of the setup screen**: every mode, every player count the grid
has room for, every vehicle - 12 values against a race actually built from them,
checking that each car gets its own grid slot with no two in the same place, and
that each vehicle arrives as the one that was chosen and drives under its own
power.

### There is a driver, and no way to race it

Worth stating plainly because the plan reads as though this were done: **Phase 8
delivered the driver and not the opponent.** `gs_ai_drive` is finished work - it
plans a line from the grip it has at that tick rather than following a baked
one, re-plans when the gravity dial moves, and is a pure integer function of the
world so a race against it replays to the bit. It is tested and it is good.

What is missing is the plumbing and the dial. In a race every car's input comes
from `gs_input_poll`; `gs_ai_drive` is called in exactly three places and none of
them is a race somebody is playing - headless self-play (`--session`), the
editor's background ghost, and the demo attract mode. A one-player race is one
car going round on its own. Phase 18 is that gap.

Two things found on the way that the work will have to face. **Both were faced -
see "Opponents worth racing" below**, and both are left here because where a
problem was found is worth as much as what was decided about it:

- **The pace dial is three constants, not a dial.** `GS_AI_CAUTIOUS`,
  `GS_AI_NORMAL` and `GS_AI_QUICK` are a fraction of available grip the driver
  is willing to use - 0.62, 0.82, 0.96. That is the right *shape* for a skill
  setting and it is three names where the rest of the game has a slider, and it
  scales only how hard the car is pushed rather than how it is driven.
- **The driver gets stuck on a bare field.** Put on a flat 40x16 rectangle with
  two gates, it laps twice and then sits in the run-off, still `active`, for
  twenty-five minutes of simulated time. Found while walking the lap dial, where
  for an afternoon it read exactly like a lap counter frozen at ten. On a
  generated circuit it fares differently and no better: dropped on the grid by
  `gs_track_grid` it finishes the race with no laps at all. An opponent that
  parks is not an opponent, and "completes any valid track" - which Phase 8
  ticks - is evidently a narrower claim than it sounds.

### Brushes meeting each other

**147 brush-and-setting configurations, every one of the 21,609 ordered pairs
walked**, in 51 seconds. What is pinned is two properties which together say a
brush is *for* one thing:

- **What it is not for, it leaves exactly as it found it.** Painting ice does
  not move the ground; a gravity pocket does not repaint the surface under it; a
  gate does not touch either.
- **What it is for, it does the same whatever was there before.** Ice is ice on
  a slope as much as on the flat, and a gravity value reads back the same over
  a ramp as over level ground.

The exception is deliberate and named: **the parts box writes several fields at
once**, because a piece lays ground, its own surface and - where it is a piece
of the route - a gate. It is exempt from the first property and held to a
different one: a piece always says what happened, and anything it changed it can
take back.

**The pair is the unit here, not the value.** Interference is a property of two
brushes meeting rather than of the number on a slider, and every one of those
numbers is walked exhaustively on its own in the option sweep - 5,563 of them.
Walking every pair of *values* instead would be a hundred million tracks to
learn what 21,609 already say.

One thing this test kept re-finding, which is now written into it rather than
learned again: **a road piece dropped on ground that already matches it is
correctly a no-op.** A straight laid on level road of its own surface is the
road it would have laid, so nothing changes and nothing is recorded - and in a
pairwise sweep the first brush leaves it exactly so surprisingly often. The
assertion is therefore tied to whether the track actually changed, not to
whether the tool said "dropped".

### The loop, closed

**A track built from nothing, undone, redone, saved, reloaded and won.** Every
test around this one starts halfway through - holding an editor and a track
somebody already made, asking one question about one brush. What none of them
asked is whether the *loop* closes.

The sequence is the one a player performs: a blank 64-square field, a ridge
raised a strip at a time, ice painted **onto the ridge** rather than beside it, a
low-gravity pocket over that, and a route. Then:

- **The validator refuses it before the route and accepts it after** -
  `GS_TRACK_NO_START` becomes `GS_TRACK_OK`, which is the difference between a
  field with scenery on it and a track.
- **Every prefix of the build survives undo and redo.** Not just the two ends:
  each undo is written down and each redo has to reproduce that exact state in
  reverse. Checking only that all the way back is blank and all the way forward
  is the finished track would pass an editor that got the middle wrong in a way
  that cancelled out.
- **What comes back off the disk is what was built**, hash for hash - the ice on
  the ridge, the gravity over it, both gates.
- **And it is raced.** Two laps on the reloaded track, won, with a finish tick
  the simulation agrees is a finish.

One behaviour pinned on the way, and it is deliberate rather than a fault:
**loading a track clears the history.** `gs_editor_load` says why in a line -
the steps in it describe edits to a track that is no longer here - so undo
cannot walk back past a load into somebody else's track. The first version of
this test assumed the opposite, undid nothing, and reported a hundred and
forty-seven corners still standing.

### The front end as a graph

The walk records where every press led, and three properties are asserted
against that graph rather than against the walk itself. Each is a thing a player
would notice going wrong:

- **No screen is a trap** - every screen has at least one thing on it that
  leads somewhere else.
- **The title is reachable from everywhere** - a screen you can leave and cannot
  get home from strands somebody as thoroughly as one you cannot leave.
- **Everywhere is reachable from the title** - the other direction, and a
  different claim. A screen nobody can get *to* is as broken as one nobody can
  leave, and only this half catches it.

`0 traps, 0 stranded, 0 unreachable`, over the eight screens the walk stands on.
Screens it merely arrived at are not asked about: one nobody departed from has
no outgoing edges because nobody looked, and asking would be asking about the
walk again.

**One exemption, named rather than counted** - and it used to be two. The
results screen is arrived at by *finishing a race*: no button anywhere leads to
it and none should.

The sign-in door was the other, because leaving it means typing a name and a
password correctly and the walk carries one word. It is not exempt any more:
the walk that does carry the vocabulary now runs as part of the same test, told
to stop the moment it is through, and the one thing it learns - that the door
leads to the title - is folded into the same graph. **No screen is exempt from
the no-trap check.**

### No control does nothing everywhere

The fourth property, and the one that needed the most finding out. A control
that never changed anything in any state it was pressed in is either dead code
or a button that lies about being one - and doing nothing *sometimes* is
ordinary, so the question is only about controls that did nothing *every* time.

`0 of 569 controls do nothing everywhere.`

**What is a control, and what is ImGui's furniture.** Of the 663 items the walk
can press, 94 are not controls and each is told apart by what ImGui itself says
rather than by a list of names that would go stale the day a column is renamed:
72 are unnamed structure - the child regions, groups and cells ImGui builds
around the widgets a person presses - 12 are window furniture, which is the
editor walk's own rule, `gs_chrome`, now shared by both walks instead of copied,
and 10 are table column headings, which sort nothing here because none of these
tables is sortable. The counts are printed, so an exclusion that starts
swallowing real controls shows up as a number that moved.

**Ten controls looked dead. Nine were the walk's fault.** The walk stands in one
state per offering, which is what lets it finish - and a radio button is inert
in exactly the state where it is already the one chosen. "Earth does nothing"
and "the walk only ever pressed Earth while Earth was lit" are the same
measurement from there, and no amount of pressing tells them apart. Standing in
every state of every offering does tell them apart, and was measured: it does
not finish in ten minutes where this finishes in one.

So a control that did nothing everywhere is taken back to a state it did nothing
in and tried again, three ways, exhaustively:

- **after every other control on that screen**, one at a time - which is what
  turns a lit preset off, picks a different row, and puts something in a box
  worth undoing;
- **with the arrow keys**, because a slider is not pressed, it is moved;
- **with every word the walk knows**, because typing a driver's own name into
  the box that already holds it changes nothing and the box is not at fault.

Nine woke up, in 256 presses: `Earth` and `baja bug` and four `untitled` library
rows after another control, `##players` by an arrow key, `undo` after another
control, and the driver `name` box by being typed something other than its own
name. The retry measures against the state it lands in rather than the hash
written down during the walk, because a seed menu cannot be built twice the
same - its driver has a password, a password is stored over a random salt, and
every state hash taken from it differs. What is checked instead is that the
control is on the screen the path led back to, which is the stronger statement.

**One is excused, by name, and the excuse is asserted.** The share code on the
tracks screen is a box you copy a track out of, drawn read-only, so nothing
anybody does to it can change anything - and ImGui does not report read-only as
an item flag when it arrives as an input-text flag, so there is nothing to tell
it apart by. If it ever stops being inert the excuse goes stale and the tree
goes red, rather than sitting there covering for whatever goes dead next.

Both halves are proved by taking them out: a button rigged to do nothing is
named and turns the test red, and removing the read-only flag from the share
code turns it red the other way.

### The walk was crediting itself with presses that could not happen

A table submits every row it holds and ImGui drops the ones outside the clip
rectangle before the widget runs - so a library of thirty-two tracks comes back
from the probe as thirty-two rows whatever the panel is tall enough to show. The
walk pressed all of them. A press on a row that has already returned sets a flag
nothing reads, and every one of those counted as a control covered.

The probe now applies ImGui's own clip test, down to the four ids it keeps alive
off-screen so a control does not die under the hand using it, and the walk
neither presses a clipped item nor counts it as offered.

- **The pinned count moves down from 750 to 663**, which is the deliberate act
  the comment beside it demands. Eighty-seven of those 750 were rows scrolled
  out of sight.
- **86 controls were drawn and out of reach**, counted where they could be
  seen rather than folded in with the ones drawn dead. Reaching them is the
  next section.
- **The walk got cheaper and truer at once**: 6,264 states and 18,979 presses
  where it was 11,825 and 26,290, and the same 100% over a denominator that no
  longer includes things nobody can press.
- **Controls reachable only by seeding went from 23 to 192**, and for a good
  reason: a seed that empties the library or leaves no track picked takes the
  detail panel away, and the table underneath grows tall enough to show rows
  that are otherwise below the fold.

### The walk can scroll

`0 controls drawn and out of reach`, down from 86, and the count of controls
covered back up from 663 to **749**.

A person reaches the bottom of a long list with the wheel, so this does: the
mouse is put over the window and a wheel event queued, which is what a backend
does when somebody turns it. It is the only mouse in the whole walk. The window
is wound to the top first - ImGui keeps a window's scroll under its name and the
walk has been through a great many screens by then - and then walked down a tick
at a time. A tick is a fraction of the panel's height, so a row would have to be
off the top at one stop and off the bottom at the next to be missed.

**It is done in two passes, and the first version was wrong in a way worth
recording.** It pressed as it went, and half the rows it reported covered had
already slid away: on this screen picking a track opens the panel underneath it,
which makes the list shorter, which makes ImGui clamp the scroll. So the
wind-down now only writes down what it saw and how far down it was, and a second
pass puts the state back, winds to that same place, and presses one row.

Forty-eight of the rows it reached then did nothing when pressed, and every one
of them was **the row that was already picked** - a track added by "Keep this
one" picks itself. That is the ordinary case the retry above exists for, so the
sweep hands those states to it, and the retry learned to wind a window too. All
forty-eight woke up.

**The seed menu is built once and copied.** Building it runs argon2 over 64 MB
twice - once to hash the driver's password and once to check it on the way in -
which is the point of argon2 and is most of a second under sanitizers. The walk
seeds from it twelve times and the passes that go back to a state were building
it again for every control they visited. Copying it instead took the suite from
125 seconds to 47, and it has a second effect worth more than the speed: a
password is hashed over a **random salt**, so two menus built from the same
instructions used to differ in those bytes and in nothing else. Copying makes
every seed byte-for-byte the menu the walk started from, so a path recorded
during the walk now leads back to the identical state afterwards - which is
asserted where it lands rather than assumed.

One test had been resting on that difference: it proved that two rosters built
from scratch are not the same state, by building two panel menus. It now makes
two drivers itself, which is what the claim was always about.

**Stopping the wheel from working puts all 86 back and turns the tree red.**

### A pad can leave a screen

Escape is a key and a pad has none of those, so somebody on a pad could only
leave a screen by walking to the button that says so - and the tracks screen
puts the whole library between them and it, one track at a time. Thirty-two
presses to get out of a screen opened by mistake.

The pad's cancel button is back as well now, **except while a race is on, where
that same button is the brake**. That is not a guess about the pad: the test
checks the binding this game ships, so moving the brake and leaving the rule
behind fails here rather than surprising somebody in the first corner. Where
back goes is still `gs_menu_back`'s to say; this is only about what counts as
asking, and it is one function with a test rather than a line in a key handler
nothing can reach.

### The count that does not come from the walk

`46 of 46` controls named in `gs_menu.c` were reached, and `30 of 30` in
`gs_editor.c`.

This is the item the rest of Phase 17 was for. `pressed == offered` was
asserted, and it is nowhere near sufficient: **the number it is out of was what
the walk itself reached**, so a walk that sees less reports all of what it saw
and calls it complete. One alphabet measured 727 controls where a wider one
measured 758, and both said a hundred percent.

The screens name their own controls, in the source text, and that text does not
care what any walk got to. So the labels are read out of the files that draw
them - every string literal in the first argument of every call ImGui reports a
name for - and every one of them has to have been met. A button added to a
screen nobody can reach is named and turns the tree red, with nobody adding a
case for it; that was checked by adding one.

Three kinds of call are left out and the reason is the same for all three:
`BeginCombo`, `Combo` and `ColorButton` never tell the hook their label, which
is why the machine choosing a paint sees sixty-four identical nameless squares.
What is inside them is covered by the sweep that opens them and by every value
of every dial being pressed.

**Turning it on found fourteen controls neither walk had ever met.**

- **Four needed a state nobody had thought to seed.** A lobby ready to race
  draws a Race button; a driver with no password yet draws SET IT AND SIGN IN; a
  server asking for a code draws the box to type it into; a track that came with
  the game draws Edit **a copy** rather than Edit. Four more seeds, and the walk
  now starts from sixteen menus rather than twelve.
- **Three were inside combo boxes nothing ever opened.** A popup is ImGui's
  state and not the menu's, so opening one changes nothing the walk can see and
  the press reads as having done nothing. It cannot be fixed by walking harder -
  standing in a state means copying the menu back and settling ImGui, and
  settling closes popups, which it has to. So the insides are swept afterwards,
  like the rows below a fold: go back, open it, press what appeared. Eleven more
  controls, and an entry that was already the chosen one is woken the same way
  as everything else - by picking a different one first.
- **Five were below the fold of the editor's own panels**: save, load, the two
  buttons that move a track as text, and the one that puts the controls back to
  their defaults. The construction set's walk had never turned a wheel.
- **Two were the same control wearing two names**, and both were faults worth
  fixing rather than test problems. The surface *brush* and the box that picks a
  ground were both labelled "surface" in the same panel, and an ImGui id is a
  hash of the label - so they shared one, and activating either reached whichever
  came first. And the rebind buttons in the controls panel change their own
  label the moment they are pressed, with `##` rather than `###`: the id changed
  with the words, so the button a person had just clicked became a *different*
  button, losing focus and active state mid-interaction, and every walk saw two
  controls where there is one.

**The pinned count is 765**, up from 750, and it is now a floor under a number
that has somewhere else to come from.

### A track built by hand, and two panels that did not fit

There was already a test that built a track from nothing and raced it, and every
step of it was a function call: `gs_editor_paint` at this tile, `gs_editor_save`,
`gs_world_add_car`. That proves the model holds together and proves nothing at
all about the construction set, because no button is pressed and no ground is
dragged over - a brush unreachable from the palette passes it.

The new one presses New on the screen about tracks, chooses each brush by
pressing its button, shapes the ground by holding the mouse down and dragging
across it, picks ice out of the surface list, winds the gravity brush down with
the arrows, clicks a route, keeps the result in the library and types a name for
it - and then races what came back out of the library. `2 gates, won on tick
735`. The mouse goes through ImGui the way a backend reports one, and where a
tile is on screen is found by moving the pointer and asking the editor what is
under it, because the projection throws away the dimension the answer depends
on.

Three things it needed that are worth recording:

- **The client's four lines.** The view carries the camera and the editor
  carries where it is looking; the client copies one into the other every frame.
  Without it the camera is a zero, every pixel maps to the same nowhere, and the
  pointer is never over any tile at all - the panels work perfectly and nothing
  can be painted.
- **Two controls on the palette are called "surface"** - the brush and the list
  of grounds it paints - so which is which is settled by pressing each and
  seeing which one offers ice.
- **The brush's gravity is not the race dial.** The planets set what a race runs
  at; the brush paints its own value into the ground, and it starts at Earth, so
  painting with it untouched would prove nothing.

**And it found two layout faults, both real, both invisible from the one state
the panel test used to measure.**

The first stopped this test dead: with nothing chosen, the box under THIS ONE
was given a height of zero, and a child asked for a height of zero does not take
none of the panel - it takes **all of what is left**. The box swallowed the space
under it and the two rows of buttons went past the bottom of the panel. It now
draws no box at all when there is nothing to put in one, which is what the
sizing already assumed.

The second came from measuring **every screen from every state the walk is
seeded in**, which is what that test now does - 96 measurements over 12 starting
states, where it was 9 over one. The setup panel was a fixed six hundred pixels
at every player count, and the grid draws a row per driver: at three drivers
thirty-five pixels were below the fold and at four, seventy-two, with the Race
button on the far side of it. The panel is now sized from its own rows.

Two more properties came out of the same work. The walk asserts that **nothing
is drawn off a panel that cannot scroll** - a control there is not scrolled past,
it is gone - and it stands at zero. And the fourth property gained a second
exemption, which is asserted as a *condition* rather than a name: **a row that is
the only entry in the library and is the one chosen**. Nothing on that screen can
un-choose it, there being no other row to pick and keeping the loaded track again
folding into the entry already there. A list of one, already selected, is the
ordinary case the rule was written to allow; the same row going quiet in a
library of thirty-two is not excused by anything.

### Every value of every dial

`106 of 106 values pressed`, on the screen rather than set in the struct behind
it. The ranges are counted out of the game's own numbers - `GS_VEH_COUNT`
machines, `GS_COLOUR_COUNT` paints, `GS_GRAVITY_PRESETS` planets, the grid's own
`GS_MAX_CARS` rows, the roster's own count of drivers - so a ninth planet is
walked the day it is added, and a screen that does not draw it turns the tree
red without anybody adding a case.

**Two planet lists became one.** The setup screen kept its own copy of the eight
planets, and the simulation exports the list the construction set's palette
draws from. They agreed to the last digit, which is how that sort of thing
survives long enough to stop agreeing. The menu uses `gs_gravity_presets` now,
and the test presses all eight in both places.

**The controls are found by what they do, not by what they are called.** Dear
ImGui reports a label for a button, a slider and a box, and none at all for a
combo or a colour swatch: neither `BeginCombo` nor `ColorButton` tells the hook
its name. So the machine picking the paint for car three sees sixty-four
identical nameless squares. What it can see is what each one *did* - press it
and the setup says which car and which colour - so that is how they are told
apart, which has the useful property of not caring what any of them is renamed
to.

**Every value is pressed from a state it is not already in.** A row already
painted red has a red swatch that changes nothing, and so does a dead one; the
two readings are identical. So the grid is moved elsewhere first and the press
has to do the work - a swatch is tried from two different starting colours, a
machine from the next machine along, a planet from the next planet.

Two things the walk cannot reach are covered here because they are dials rather
than destinations: the lap slider is drawn **dead on a path**, since a path is
raced once end to end, so the test lays a circuit before asking for twenty laps;
and both sliders are driven by landing on them and stepping with the arrows,
which is what a person without a mouse does and where a person with one ends up.


### Half the modes had the other one's HUD

Nobody had looked at a demolition derby. It is one of the two things the mode
dropdown offers, and the screen it draws was built for the other one:

    1/4   position          two of the four already wrecked
    1     lap               there are no laps
    10.00 this lap          a lap clock, counting nothing
    -     best              a best lap
          condition         the only row that meant anything

**Four of five rows were about getting round a track**, which in "last one
driving" decides nothing: a car three corners ahead and a car sitting still are
equal until one of them is wrecked. And the one question the mode does ask - how
many are left - was not on the screen at all.

It shows what it is about now: `still driving`, and the damage bar. Two rows,
and the panel is two rows tall, because the panel's height and its contents are
one list in this file rather than two that drift.

**The count has one definition.** It was already being worked out in the rule
that ends a derby, so `gs_world_driving` is that count, and the rule calls it -
two definitions of "out of it" is a screen saying two are left over a race that
has already been won.

The HUD test now renders **twelve** states rather than seven: both modes, wrecked,
finished, waiting, counting down. And it asks two questions of each rather than
one - what fell off the bottom, and *how much of the panel was nothing*. A panel
sized for rows it is not drawing has a hole in it, and no amount of asking what
overflowed can see a hole.

### What a new player sees

Nobody had ever looked at the first screen of a fresh install, because this
machine has had a driver on it since the door was built. Running with an empty
preferences directory shows it, and it was wrong in the way that matters most:

**SIGN IN was the big blue button, and on a machine where nobody has a driver it
cannot succeed under any name or any password.** Three times the size of the
button beside it, first in the tab order, and guaranteed to fail - and what it
says when it fails is that the driver does not exist, which reads like the game
refusing somebody who has done nothing wrong. The thing they had to press was
the quiet one underneath.

There is nothing to sign in to on an empty roster, so the boxes are gone too.
One sentence and one button:

    GEARSTICK
    who is driving?

    Nobody has driven here yet.

    [ NEW DRIVER ]
    [ Exit ]

The panel is shorter to match, because a panel sized for a form that is not
there is a rectangle of empty screen. The roster being empty is a settled fact
rather than something that changes while somebody is looking at it, which is why
this may depend on it where the tracks screen's list may not.

**And the walk now starts from that state too** - seventeen seed menus rather
than sixteen. It has to: the count taken from the source demands that every
label the screens draw is reached, and `NEW DRIVER` is a label no other starting
state can produce. That is the check doing exactly what it was built for.

### How to photograph a screen, since it keeps being worth it

    SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software SDL_AUDIO_DRIVER=dummy \
        gearstick --screen setup --shot out.bmp

`--screen` takes any of login, title, drivers, setup, tracks, records, lobby,
results; `--editor` and `--heatmap` open the construction set; `--players N`
and `--shot-at TICK` set the grid and how far into the race to wait. **The file
is a BMP whatever the name says.**

States the flags cannot reach - a track picked, a lobby with people in it - are
reached from a test instead: `gs_panel_of` draws any screen from any menu, and
`SDL_RenderReadPixels` plus `SDL_SaveBMP` writes what it drew. That was worth
building for an afternoon and is not worth keeping as a test, because nobody
looks at a picture a test writes.

**Six faults came out of doing this**, none of which any test could see:

| what the picture showed | what it was |
| --- | --- |
| Venus and Jupiter cut in half | a panel narrower than its contents |
| `Escape  quit` on the title | a pad's cancel closing the game |
| the palette ending at "Route" | save, load and the help line below the fold |
| `position 3/4` on the grid | race order decided by which lane you are in |
| the preferences directory written a minute ago | the suite overwriting a player's track |
| two cars for `--players 4` | a flag silently ignored in one mode |

The pattern is worth naming. The tests answer **can it be reached** - they press
every control in every state, by name, and assert coverage against the source.
They cannot answer **is it right when you look at it**, and a machine that
presses by name and winds panels to reach things is structurally the worst
possible judge of whether a thing is findable at all.

**And then it stopped finding things**, which is the other half of the report.
Six more states were photographed and every one of them was right: the editor's
controls panel with all four players and the button that restores the defaults;
the new-driver form, with the caret already in the name box; choosing a password
for a driver who has none; the setup screen refusing an unsound track, with GO
drawn dead and the reason beside it; the records table with times in it; and the
lobby with four people in it. Nothing to fix in any of them.

So the screens have now been looked at, most of them in more than one state. The
one visible artefact left is in the instrument rather than the game: a menu drawn
from a test has ImGui's keyboard-nav cursor sitting on its first item, which the
game does not show until somebody navigates. Photographs taken through
`gearstick --shot` do not have it.

### The suite got better at pressing buttons and started deleting people's work

The construction set saves the track being built, and the controls panel saves
the bindings somebody has chosen, into the preferences directory - the real one,
where a player keeps their things. The walk presses every control in the editor.

Those three buttons - `save`, `load`, and the one that puts the controls back to
their defaults - sat below the fold of a panel too short to show them, so for as
long as that was true, nothing had ever pressed them. **The day the walk learned
to wind a panel down, running `ctest` began overwriting a real player's current
track and their bindings.** It went unnoticed because what it wrote happened to
match what was already there; on the next machine it would not have.

Every test now runs with `XDG_DATA_HOME`, `HOME` and `APPDATA` pointed at a
throwaway inside the build tree - which is what `tools/play_check.py` has always
done for the clients it starts, and what the C tests never did. And the render
suite says so out loud: it checks that the directory it would write to is that
throwaway. Point it anywhere else and the tree goes red instead of somebody
losing what they were building.

This is the second time in an afternoon that **making the tests better made them
dangerous**, and both times in the same way: a walk that could not reach a
control was, accidentally, a walk that could not break anything with it.

### And a fourth, which was a rule in the simulation

Photographing a four-car race showed the HUD telling the player on pole, before
the lights had gone out and before anybody had moved, **position 3 of 4**.

`gs_world_place` orders cars by how far round the route they are: laps, then
which gate they are heading for, then how close they are to reaching it. That
last part measured the **straight line to the gate's centre point** - and a gate
is a line across the road that a car crosses wherever it likes. So the car in
the middle of the road was nearer the gate than the car level with it on the
outside, and four cars sitting abreast on a standing grid came out third, first,
second and fourth.

It measures the part of the leg still to travel now, projected onto the leg, so
two cars level across the road have identical progress and the tie is broken by
index, which is stable.

**Why no test caught it.** Every test of this rule put its cars at different
distances *along* the track, because that is the interesting case when you are
thinking about overtaking. Not one of them put two cars level. The test does
now, in the two forms that matter: four cars abreast on the grid, and four
abreast a third of the way down a leg - and moving one *across* the road changes
nothing at all, which is the fault stated as a property.

The golden replay did not move, and could not: `gs_world_place` is a question
asked of the world rather than part of stepping it. That is the separation those
two hashes exist to keep.

### And a third, in the construction set

Photographing the editor - which is half the product and had never been looked
at in a picture - showed the palette ending at the route check. **Save, load,
the two buttons that move a track as text, the controls checkbox, and the one
line that tells a new player what the mouse does** were all under the bottom
edge of a window with two hundred and forty pixels of empty screen beneath it.

    ready
    Tab races it. Arrows pan. Drag to paint.

That sentence is the most useful thing in the construction set and nobody had
seen it. The walk had pressed every one of those controls, by name and by
winding the panel - which is exactly why it did not notice.

The palette is as tall as the window allows now, and the route list scrolls
inside a box of its own, the way the library and the lobby already do: a track
can have any number of gates on it, and a panel whose height depends on that is
a panel that is the right size until somebody lays another one.

**The rule that keeps it that way** is asserted in every one of the editor
walk's 46 configurations, and it is not "nothing scrolls" - that would be a lie
for an unbounded list. It is: *a panel may only hide something if it is already
using the whole screen*. What was wrong was never the scrolling; it was
scrolling with a quarter of the screen empty underneath.

### And a second thing, from reading the screen rather than measuring it

The title screen prints its own key list: `Tab  the construction set`, `Escape
quit`. Giving a pad's cancel button the same job as Escape therefore gave it
that one too - **B on the title screen closed the game**, with no warning and
nothing said about it anywhere, and B is the button people press reflexively to
go back one step.

Only a key can ask to quit now. `gs_input_back_may_quit` says so in the platform
layer, beside the rule about what counts as asking to go back, so it is a
sentence with a test over it rather than a condition in a frontend nothing can
reach. Where there is nothing behind the screen - the title, and the door - a
pad's cancel does nothing at all, and Escape does what the screen says.

### A photograph found what none of the numbers could

The setup screen gained a skill dial beside the driver count, and it fitted:
nothing below the fold, nothing off the bottom, the walk pressed all 765
controls, every panel measured clean over sixteen starting states. Then a
screenshot of the real client showed **Venus and Jupiter ending in the middle of
their own names** - the left-hand column had grown, the gravity buttons had gone
with it, and the last two were drawn thirty-three pixels past the panel's right
edge with nothing to press on the other side.

**Nothing in the item hooks can see that.** ImGui's clip test is an overlap, so a
button hanging off the edge is still "visible" and still answers to being pressed
by name. Reporting whether an item's rectangle sits inside its window does not
help either: what the hook is handed has *already been clipped*, so the button
arrives looking like a narrower button that fits.

What sees it is the window's own content size. `gs_panel_report` now carries
`wider` - `ScrollMaxX`, the horizontal twin of the `hidden` it has always
carried - and the panel test asserts it is zero for every screen from every
seed. Removing the dial takes it to zero; putting it back reports 33 in every
state. The panel is 800 wide now rather than 760.

The lesson is the one the project already wrote down and this is the first time
it has cost something: **verify on the real output**. Every proxy said yes.

### Two tests that were skipping now run

`gearstick_transport_document` and `gearstick_noise_interop` both need the
`noiseprotocol` Python package and both quietly skipped without it. A skipped
test is not evidence, and the first of those is the only automated check the
transport specification has.

Installed on 2026-08-27, and both pass. The document check writes a client from
`docs/TRANSPORT.md` and nothing else, completes a handshake with a real server,
and confirms the byte sizes the document quotes: **96 bytes for the first
handshake message, 48 for the second, and 38 for a sealed datagram carrying
eight** - six of header, eight of counter, the payload, and a sixteen-byte tag.
So the prologue, the key schedule and the framing are right *as written*, which
is what a document is for.

It is still not the item's verification. The person who wrote the document also
wrote the code and cannot unsee it; what is missing is a reader who has not.

**All nineteen tests now run.** There are no skips left in the suite on this
machine.

## Opponents worth racing

There was a driver in this game and no way to race it. `gs_ai_drive` has been
finished and tested since Phase 8, and every car in a race took its input from a
pad - so the AI drove in headless self-play, the editor's background ghost and
the demo, and nowhere a player could see. **A one-player race was one car going
round on its own.**

### The empty seats

A slot on the grid is the game's until somebody takes it. The driver list on the
setup screen offers **computer** alongside the guest and the roster, and it is
what an empty slot starts as - the second car is somebody by default, because
starting a race and finding one car on the grid is the thing this exists to
stop.

What the setup screen *means* now lives where a test can reach it:
`gs_setup_build` builds the race the screen describes and `gs_setup_drive` fills
in the slots nobody is driving, both in `gs_menu.c` rather than in the client. A
race for four with nobody at the keyboard finishes with three cars timed and an
opponent winning it, and the parked car is left exactly as it came in - filling
in a slot somebody is driving would be the game taking the wheel off them.

The game's own cars stay out of the records. A table with the computer at the
top of it is a table nobody can get on.

### Skill is a dial

Twenty-one settings, from a driver who brakes far too early to one who is
quicker than you are. The three names that used to be the whole of it -
cautious, normal, quick - are three points on it now, and the construction set
and the race setup read the same list of planets for the same reason.

`84 lap times, every step strictly quicker than the one below it, no ties`, in
four sets of conditions: pavement, dirt with two thirds of the grip, the Moon
with a sixth of the weight, and a different machine. The driver is not tuned per
track, so a dial that only works on the track it was tuned on is not a dial.

**It moves three things together, because they are the same confidence:**

- **how much of the grip they ask for** in a corner, which is what it always
  was;
- **where they lift** - a fifth early at the bottom, on the sum at the top;
- **how straight they hold it** - a wide dead band wanders and corrects late, a
  narrow one keeps the car pointed.

That is what makes it a driver rather than a handicap. The two closest settings
on the whole dial lap 25 ticks apart over three laps and still leave the same
ramp at different speeds and land in different places; across the dial the timid
one arrives at the corner at a sixth of the speed. **No two neighbours take the
jump the same way.**

The braking margin is also what made the dial monotonic. With one knob, dirt had
an inversion: a step up cost more in the slide than it gained on entry. With the
lift point moving too, every step is quicker on every surface.

### Three faults in the driver, found by racing what ships

- **A hairpin was read as a straight.** The radius of a turn is
  `leg·cos(half)/(2·sin(half))`, which goes to nothing as the corner approaches
  a full reversal - and `cos(half)` reaches exactly zero at a hundred and eighty
  degrees. That case was lumped in with "straight on, or as near as makes no
  odds", so a driver arriving at a hairpin planned no braking at all, went
  straight on at the gate, and spent the rest of the race looping back for it.
  **Every track with two gates on it is that corner**, which is why a bare
  rectangle with a start and a finish was where it showed.
- **A step too steep to climb was driven into rather than round.** The ground is
  authored per corner and nothing stops a whole tile of rise in one tile - the
  parts box drops pieces like that and the generator makes them. The way to the
  gate is now sampled ahead, and where it crosses a wall the aim turns away from
  it an eighth of a turn at a time, nearest first, until it finds a way past.
  Asked of the way to the gate rather than the way the car is pointing, because
  a test on the heading changes its mind every time the car turns and the two
  then argue until the race ends.
- **A car pinned against a cliff stayed there.** Steering is something a moving
  car does, so full power into a three-tile face held it at nought point nought
  one tiles a second for three minutes. It backs off now - with the wheel the
  other way, because reversing turns the nose the opposite way and the first
  version of this oscillated off the wall and back into it.

`88 races over the 22 tracks that ship, from every grid slot, none stuck`, and
48 more over twelve generated tracks. Every slot, because the grid is staggered
back from the line and across it, and the car in the last one has a different
first corner to make.

### And it replays to the bit

The golden replay has a second race in it: four opponents spread across the
dial, on a circuit none of them has seen, with **no recorded inputs at all** -
the driving is worked out again from the world each time. `gearstick_cli
selftest --verify` races it, drives it a second time and gets the identical
world, replays the recording of it and gets the same again, and compares the lot
against a pinned hash.

That number moves for two reasons rather than one. The physics, like the number
above it - and *the driver*, because an opponent is a pure function of the world
and a change to where it lifts is a change to every race anybody recorded
against it.

## Half the planets, at a window somebody can drag to

**Sideways is a direction, and the panel test only looked down.**

Every one of these screens is a window that cannot be moved, resized or
collapsed, clamped to the viewport with a margin, and the promise it makes is
that whatever does not fit *scrolls*. That promise was half kept. Vertical
overflow scrolled and had a test on it - `hidden`, the window's `ScrollMaxY`,
which is what caught a Race button below the fold at four drivers. Horizontal
overflow had nowhere to go: `gs_centre_window` clamped the width and the
difference was simply thrown away.

At 640x480 - the size the panel test itself calls "what somebody dragging a
corner gets" - the race setup screen wants 800 and gets 624. **Mars, Venus,
Neptune and Jupiter were not on the screen**, and neither were the last two
paint colours on every driver's row. Four of the eight worlds you can race on
and two of the eight colours you can be, gone, with the screen looking entirely
normal.

The `wider` assertion added when Venus and Jupiter were cut in half would have
caught it, except that it only ever ran at 1280x720, where the panel fits.

### Two different faults wearing the same face

Fixing the first one is a flag: every panel is opened with `GS_PANEL_FLAGS`
now, which is the old set plus `ImGuiWindowFlags_HorizontalScrollbar`. A window
whose contents are wider than it is gets a scrollbar, the wheel and ImGui's own
navigation both reach it, and the gravity buttons come back.

That fixed the planets and not the paint. **A stretched table column does not
overflow - it shrinks**, and then clips what no longer fits inside its own cell,
where the window's scrollbar cannot reach it. The grid's `paint` column was
`WidthStretch`, so at a narrower window it quietly got narrower and took two
swatches with it. It is given an explicit width now: past 800 the table still
stretches to fill the window; below it, the table keeps the width the screen was
designed at and the difference becomes something the panel can be scrolled
sideways to. The chrome is measured rather than assumed, because whether a
vertical scrollbar is taking fourteen pixels depends on the driver count.

### The test is the sweep, not the number

`at_the_smallest_window_every_control_can_be_scrolled_to` states the rule the
way a player would: **at 640x480, every control on every screen is wholly on
screen at some scroll position the window can actually be put at.**

Each screen's panel is put at every scroll position on a grid half a viewport
apart in both directions - close enough that nothing narrower than the window
can hide between two of them - and the whole frame is laid out at each. What
comes back is, per control id, whether any of those positions ever showed all of
it. `whole` was already there; what was missing was moving the window and asking
again.

It reports what it covered: **121 controls across 8 screens**, plus 77 that sit
inside lists which scroll themselves and are walked by `gs_walk_reach` instead.
Before the fix it named four; after it, none.

Two things were needed to write it. `gs_ui_probe_scroll_span` and
`gs_ui_probe_scroll_to` read and set both axes, where the old
`gs_ui_probe_scroll_at` was vertical-only - the same blind spot as the panels,
in the instrument. And the sweep cannot use `gs_ui_controls`, which leaves the
harness pointing at a `gs_menu` inside its own stack frame: fine for a caller
that begins again before its next frame, a read of dead stack for one that keeps
framing. That was a hard crash under ASan with no usable trace, and it is worth
knowing it is there.

**No physics moved.** The golden replay is untouched; this is all `src/ui/`.

## CI was red on two platforms and nobody was looking

`ctest` was green on this machine for four commits in a row while **every
Windows build failed to compile and every macOS run failed a test.** The rule
in CLAUDE.md is that a red tree stops everything; it says nothing about a tree
that is only red somewhere else, and that is exactly the gap.

### Windows: one cast, in a file the simulation lives in

```c
in &= (gs_input)~(unsigned)(GS_IN_LEFT | GS_IN_RIGHT);
```

`gs_input` is a byte. `~` on the promoted `int` makes a constant with the top
twenty-four bits set, and casting that down to a byte truncates it - which is
what was meant, and is warning C4310 to MSVC, and warnings are errors here. gcc
and clang say nothing at all, so nothing on this machine could see it. The
complement is taken inside the width it is stored in now.

**Nothing on Windows built for four commits**, which also means none of the
Windows tests ran, which means the only thing that would have caught anything
*else* platform-specific was also off.

### macOS: the sandbox that only works on Linux

The suite presses every control in the construction set, two of which save a
track and the key bindings into the preferences directory, so every test runs
with `HOME`, `XDG_DATA_HOME` and `APPDATA` pointed at a throwaway inside the
build tree - and `no_test_writes_where_a_player_keeps_their_things` says so out
loud.

On macOS none of those three does anything. SDL asks the platform where a user's
things live and the platform answers from the password database, not from the
environment. So the run wrote to the real `~/Library/Application Support` and
the test that exists to catch exactly that went red there, correctly, and stayed
red.

`gs_pref_dir` reads `GEARSTICK_PREF_DIR` now, before it asks SDL anything. One
override, the same meaning on all three platforms. A portable install - a copy
on a memory stick keeping its preferences beside itself - gets it for free.

### And the sandbox is in the binaries, not only in ctest

The environment was set by `set_tests_properties`, which covers `ctest` and
covers nothing else. **Running a test binary straight from the build directory
is what anybody debugging one does**, all afternoon, and that run had no sandbox
at all - which is how a player's `current.gstrack` and `controls.gsbind` came to
be rewritten during this very session. They happened to be byte-identical to
what the walk writes, which is the same luck the original fault had.

`tests/gs_sandbox.h` sets the override from every test main that links SDL, with
`overwrite` zero so CMake's choice still wins. Not from the ones that link none
of it - `gearstick_tests`, `gearstick_store_tests`, `gearstick_noise_tests` -
because a binary with no SDL cannot call `gs_pref_dir`, and pulling SDL in to
say so would break the layering the whole project rests on.

## A HUD too big for the view it is in

Every state of the HUD had been measured - twelve of them, race and derby,
wrecked, waiting, finished - and asked both halves of the question: did anything
fall off the bottom, and is there a hole where a row is not being drawn. All
twelve were clean.

**All twelve were measured in one view filling the whole window.** Four players
do not get that. The window splits four ways and each view is a quarter of it,
and the HUD was positioned inside its view and then sized from its rows and
nothing else. At the size the game opens at, a quarter view is 638x358 and the
plainest race HUD is 331 tall - and **six of the twelve states were taller than
the view**, drawn straight over the player below and reading them somebody
else's lap time. At 640x480 every one of the twelve overflowed, by up to 259
pixels.

ImGui clamps a window to the viewport, which is the whole screen. It has never
heard of a view.

### The panel is a fraction of itself now

Not the text alone: the text, the gaps, the window padding, the condition bar
and the width all scale together, so a quarter-screen HUD is the same HUD
smaller rather than the same text in a squashed box. `gs_hud_height` takes every
size it is built from as an argument, which is what makes that one call rather
than a redesign.

There is deliberately no legibility floor. The rule is that the panel stays
inside its view, and a floor would be a rule that holds until somebody drags the
window small enough to break it.

### The pixel per row that ImGui rounds away

The first attempt scaled the numbers and left a fourteen-pixel hole at the
bottom of the box, which the existing test caught. **ImGui bakes a font at whole
pixels**: a line asked for at 12.28 comes back at 12, measured rather than
guessed at -

```
FONT base 13.0000
FONT x0.9446  want 12.2798  got 12.0000
FONT x0.7500  want  9.7500  got 10.0000
FONT x2.2000  want 28.6000  got 29.0000
```

- so it rounds to nearest, and a panel sized from the number it *asked* for is a
pixel per row too tall. Fifteen rows of that is the hole. `gs_hud_line` rounds
the same way the renderer will, and because that makes the height not quite
proportional to the fraction, the fraction is found by dividing and then
stepping down a hundredth at a time until it fits.

### The test, and the one it broke

`a_hud_stays_inside_the_view_it_belongs_to` puts four cars in the four corners
of a big track, splits the window four ways, and draws all twelve states in all
four views **at both 1280x720 and 640x480**: 96 panels, each required to sit
inside the view it belongs to. Named that way because a HUD outside its view is
not a HUD that overflowed, it is a HUD in somebody else's screen.

It also broke a test about chequered paint, which is worth writing down. The
window is one object shared by every test in the binary, and the ones that read
a frame back index it as `GS_W` wide - so a test that resizes and does not put
the window back does not fail itself, it fails whatever runs next.
`a_start_line_and_a_finish_line_are_different_things` lost five checks at once
to a HUD test forty lines away. Every size-changing test puts the window back
now.

### Both sweeps then walked their whole space, and found nothing more

Two coverage holes were left behind by the two fixes above, and closing them is
the difference between a test that caught one fault and a test that will catch
the next.

The small-window sweep ran from one starting state, and how big a screen is
depends on what is on it - the setup screen grows a row per driver, the tracks
screen draws a detail panel only once something is chosen, the door is a
different door with an empty roster. It runs from all seventeen states the
panels are measured from at full size now: **1758 controls over 136
screen-states**, every one of them reachable at 640x480.

The HUD sweep used four players, and the screen is divided by the car count:
two get half the window each and keep its full height, three and four get a
quarter. A HUD that fits a quarter fits a half, but a HUD that fits neither is a
different fault in each. All three counts are walked now: **216 panels**, twelve
states by two, three and four views by two window sizes.

Neither found anything. That is the result, and it is worth as much written down
as a fault would have been - the fixes hold across the whole space and not only
where they were found.

**No physics moved.** This is all `src/ui/`.

## A scrollbar that scrolls nothing, and the widest name anybody can type

Two more came out of photographing screens, one of them mine from an hour
earlier.

### The furniture I had just added

Giving every panel `ImGuiWindowFlags_HorizontalScrollbar` fixed the four missing
planets and put a **permanent scrollbar with the grip filling the whole track**
along the bottom of screens that fit perfectly well. A window carrying that flag
shows the bar whenever its contents are as wide as it is - and a `Separator`, or
anything else drawn at the full width, *is* exactly as wide as it is. Fourteen
pixels of furniture that scrolls nothing, on the tracks screen and anything else
with a rule across it.

None of the numbers could see it: `ScrollMaxX` was zero, which is exactly why the
grip filled the track. A photograph could.

`gs_centre_window` asks for the flag only when it actually took width away,
which it knows before anything is drawn - so the bar exists precisely when the
panel is narrower than it wants to be, and never otherwise.

### And the test could not have caught it, or the fault before it

`at_the_smallest_window_every_control_can_be_scrolled_to` reaches a control by
setting the window's scroll position, and a test can do that to any window
whether or not it has a scrollbar on it. So it proved every control was
*somewhere*, and not that anybody could get to it - it would have passed with
the flag removed altogether.

It asserts both halves now: a window that can be scrolled shows the bar that
says so, and a window that cannot does not. That is one rule catching the
missing planets and the useless scrollbar at once, and it holds over all 144
screen-states.

### The widest name anybody can type

A driver name holds fifteen characters, a track name forty-seven, an author
twenty-three. Every layout in this game had been measured with "gavin" and
"track number 7" in it - the narrowest a screen can be, and a name is the one
piece of a screen the person using it chooses the width of.

`the longest names that fit` is an eighteenth starting state: every name field
in the menu, the library, the records and the lobby filled with W - the widest
glyph there is, because what has to survive is not a plausible name but the
widest one the field will hold.

**Nothing broke**, at either window size, in any of the eight screens. Written
down because it is a class of fault that was never being looked for, and now is
- 144 screen-states measured at full size and 1865 controls checked for reach at
640x480.

## The construction set was laid out for one screen

The editor is half the product and its three panels were placed and sized from
constants chosen while looking at a 1280x720 window. Measured anywhere else:

| window | what happened |
| --- | --- |
| 960x600 | the parts box **304 pixels off the right-hand edge**; the palette and the controls 104 below the bottom |
| 640x480 | nineteen pixels of the parts box on screen and the rest not |
| 400x300 | the palette 377 pixels taller than the screen |

And **nothing scrolled**, in either direction, on any of them.

That last part is the interesting one, because it is why none of the numbers
already being taken could see this. `every_control_in_the_construction_set_is_pressed`
has watched these panels for things below the fold since the day save and load
were found hiding under one - and it asks the *window* what it is hiding. A
window knows what did not fit inside itself. It has no idea it is hanging over
the edge of the display: ImGui clips it there and never tells it. Every one of
those panels reported nothing hidden while most of it was off the screen.

ImGui keeps about nineteen pixels of any window reachable so it can always be
dragged back into view. That is a rescue, not a place to open in.

`gs_editor_panel` now opens all three: never bigger than the screen, never
draggable or resizable past its edges, put back inside if the screen shrinks
under them, and carrying a sideways scrollbar exactly when the screen is
narrower than the panel wants to be. What the player does inside that is still
theirs - these are tool windows and they stay movable.

### The test, and the one it broke - again

`the_construction_set_keeps_its_panels_on_the_screen` measures all three panels
under **every brush**, because the brush decides what the palette holds, at four
window sizes down to 400x300: 72 measurements, each required to be wholly on the
screen and to show a scrollbar exactly when it can scroll.

It also broke every editor test after it, in places that mention no windows at
all. **ImGui remembers a window's position and size under its name for the rest
of the process**, so a test that shrinks a panel to fit a 400x300 screen has
shrunk it for everything that follows - and the walk then found 65 of 66
controls instead of 69 of 69, with 134 panels hiding things. This is the same
hazard as resizing the screen and not putting it back, one level deeper.

`gs_ui_probe_place` puts a window back. The test records where all three were
before it starts and restores them at the end, then checks they really did go
back - because a cleanup nobody verifies is a cleanup that stops working
silently.

### Every control on those panels, too

The same standard the front end's panels are held to: each panel is put at every
scroll position on a grid half its own size apart in both directions, and every
control on it has to be wholly on the panel at one of them. **1548 controls over
the 72 measurements**, none of them cut in half. Anything drawn inside a *list*
on a panel scrolls with the list rather than the panel and is walked by
`every_control_in_the_construction_set_is_pressed` instead, which the test says
out loud rather than folding into a number that would then mean something else.

### And a compiler difference caught the way the last one was not

`e.brush = (gs_brush)brush` is fine to gcc and an error to clang: **an enum is
unsigned to one and signed to the other**, and `e.brush` is an `int` because
that is what ImGui edits. Green here, red on macOS, exactly like the MSVC cast
two hours earlier - and this time it cost a red main rather than four commits,
because CI was being watched.

There is a `build/linux-clang` now: the whole tree, built by clang-19, as a
second opinion before anything is pushed. The `parsers, fed rubbish` job already
used clang, but it builds the fuzz targets and not the tests, so it never saw
this file. Two compilers locally is not three platforms and does not pretend to
be; it is the cheapest way to catch the class of thing that has now bitten
twice in one day.

### Every window, not only every panel

There are ten `ImGui_Begin` sites in this game - eight menu screens, the
editor's three panels through one helper, and the HUD - and all of them are now
measured. But a *window* is not the same as a `Begin`: a list, or a bordered box
inside a panel, is a window of its own to ImGui, with its own clip rectangle and
its own scroll. There are three of those, and none of them had ever been asked
what it was hiding sideways.

The scrollbar rule applies to all of them now, discovered from what the frame
actually drew rather than from a list somebody keeps up to date: **183 windows**
across the eight screens and eighteen starting states at 640x480, each required
to show a scrollbar exactly when it has something to scroll.

Nothing was wrong. The number that matters for believing that is the other one
the test prints: **27 of the 183 can move sideways and 93 can move down**. A
rule about showing a bar when you can scroll proves nothing in a sweep where
nothing can scroll - it would be asserting that no window has a bar, which is a
different and much weaker claim - so the test fails if either count is zero.

**No physics moved.** This is all `src/ui/`.

## Six of the nine grounds sounded like pavement

`gs_surface_voice` had three cases and a `default`. It was written when there
were three surfaces. Six more went in - sand, gravel, rock, dust, slush, grass,
each with its own grip, its own rolling resistance and its own way of wearing -
and **every one of them fell through the default and sounded exactly like
pavement.**

Two thirds of the grounds in a game whose editor lets you paint all nine, in a
game where what you are driving on is the point. The test next door says so in
its own name: *the ground a car is on changes what it sounds like*. It walked
pavement, dirt and ice, which were all there were when it was written, and it
went on passing.

That is the sampling failure the project already has a rule against, and this is
what it costs. The rule says cover every scenario, not a representative sample.
Three of nine was a representative sample.

### No `default`, ever, on a switch over an enum

The structural half of the fix, and the more important one: the switch has a
case per surface and **no default**, so `-Wswitch` makes a tenth surface a build
failure rather than a tenth surface that sounds like the first. `gs_screen_name`
in `gs_menu.c` was already written this way; this was the only place in the tree
that was not.

The numbers themselves are a judgement rather than a derivation - what grass
sounds like is not in its rolling resistance - but they are read off the
character each surface is given in `gs_track.c`, and no two are alike in either
column.

### And what the test asserts now

Every surface, walked from `GS_SURF_COUNT` rather than a list, so a tenth is in
the test the day it exists. Each one has to be audible at all, and then **all 36
pairs have to be tellable apart** - by a tenth in loudness or ten zero crossings
in brightness - because two surfaces that sound alike means the second one is a
colour in the palette rather than something to drive on.

The named facts stay: dirt is the loudest of the nine and ice the quietest, ice
is a hiss where dirt is a rumble, rock is the deepest thing here. What is *not*
asserted, deliberately, is an order across all nine by brightness. Loud and
bright are two axes rather than one; sand and gravel are thrown against the
underside and read sharper than ice does, because there is far more of them, and
ice is the quietest thing here. Ranking all nine would have meant inventing a
design decision to fit whatever the numbers happened to be.

### The headroom checks were sampling too

`nothing_the_synthesiser_produces_can_blow_a_speaker` used dirt in a sprint car,
dirt having been the loudest surface when it was written. **Three of the six new
ones are louder than dirt.** It walks all nine grounds in all six machines now -
54 mixes, four cars each, all sliding and all being hit - and the music-under-a-
race check walks all nine with a different tune under each. Nothing clipped.

### Two more compiler differences, caught before pushing

`build/linux-clang` earned itself twice in one sitting: `gs_music_start(0x1234ULL
+ (uint64_t)surf)` and `mixes == GS_SURF_COUNT * GS_VEH_COUNT` are both fine to
gcc and both errors to clang - a sign change on one, arithmetic between two
different enumeration types on the other. Neither reached CI.

**No physics moved.** `src/audio/` is downstream of the simulation and no part
of it; the golden replay is untouched.

## Nothing had ever read a gamepad

Built the whole tree under clang's source coverage, ran `ctest`, and asked which
first-party files the suite never touches. `src/platform/gs_input.c` came back
at **24% of its lines**: opening a pad, closing one, hotplug, and every line
that reads a physical control had never been executed by any test, on any
platform, once. Everything this game says about pads - four players on one sofa
is the shape of it - rested on code nothing had run.

The reason is obvious once seen. `gs_bind.c` is 98% covered, because resolving
"which buttons are down" into "what this player is asking for" is a pure
function and was deliberately written to be testable without hardware. The half
that talks to SDL was the half nobody could test, so nobody did.

**SDL's own answer is a virtual joystick.** It is a real gamepad as far as every
call in `gs_input.c` is concerned - opened, polled, closed - and its buttons are
set from the test instead of by a thumb.
`a_pad_is_opened_read_and_closed_the_way_a_person_plugs_one_in` plugs in four,
one at a time, and checks:

- a fifth is refused rather than remembered, because there are four cars;
- pad N drives car N **and only car N**, walked over every pad rather than
  checked on one, because that is the whole claim and it is per pad;
- every button bound by default does what it is bound to;
- both triggers stand in for the two buttons everybody drives with;
- the stick steers past the deadzone and **not before it** - a stick resting
  slightly off centre must not steer, or a worn pad drives into a wall on its
  own, which is the entire reason there is a deadzone;
- somebody trips over a cable mid-race: the hole closes, the pad that was third
  drives the second car, and nothing reads a closed pad afterwards.

24% to 75%.

### Two ways to lose twenty minutes, both worth writing down

**`gs_input_poll` adds the keyboard to the pads** rather than choosing between
them - deliberately, so a pad and the arrow keys can drive the same car and
neither disables the other. In a test binary that means every key event an
earlier test left in SDL's queue, applied the moment anything drains it and then
held forever because the matching key-up was never queued. It arrives as car one
accelerating and reads exactly like a pad driving the wrong car.

**A released trigger is not zero.** A gamepad reports a trigger over 0 to 32767
and SDL maps that from a joystick axis whose range is -32768 to 32767 - so
writing 0 to the axis, which is the obvious way to say "let go", is a trigger
held at half travel. Which is over the threshold, and reads as accelerate and
brake held together on car one, and looks exactly like a pad driving the wrong
car. Letting go is the minimum. The test asserts it now: letting go really lets
go.

Neither was a fault in the game. Both looked like one.

### What the coverage build is for

`build/linux-cov` is clang with `-fprofile-instr-generate -fcoverage-mapping`.
It is not part of `ctest` and is not meant to be a number anybody chases: what
it is good at is answering "what has never run at all", which is a different
question from "what is tested" and the only one a coverage tool answers
honestly. It found this in one pass.

It also reported `src/frontend/game/main.c` at 0% of 1186 lines. **That number
was wrong and is corrected below** - it is a property of how the client is
stopped, not of what runs it.

## The 0% that was not, and two ways out of one screen

`src/frontend/game/main.c` was reported here as 0% of 1186 lines, "never entered
by any test in the suite". **That was wrong.** `gearstick_plays` and
`gearstick_front_door` both drive the real client through real races, and both
stop it with `proc.kill()` - SIGKILL, deliberately, so the harness behaves the
same on every platform. A process killed that way never flushes its coverage
counters. The client runs; nothing can see it run.

Measured properly - five clean-exit invocations of the client through its own
flags - main.c is at **34%**, and the parts that stay dark are the ones the two
end-to-end checks reach and the flags do not.

The lesson is the one this project keeps relearning from the other side: a
number that says "never" is a claim about the instrument as much as the code.
The pad finding a few hours earlier was real because `gs_input.c` is compiled
into a test binary that exits normally.

### And what was behind it: Escape did not say what the screens say

Looking at what main.c does with Escape led to `gs_menu_back`, which is where
the rule actually lives - "where back goes is gs_menu_back's to say, not this
handler's, so that it is a rule with a test rather than four lines nothing can
reach". Two screens disagreed with it.

**The records screen** is opened from the title, from the setup screen and from
the results, and it remembers which so its Back button can return there. Escape
ignored that and went to the title. A player who opened the records from their
own results and pressed Escape - or a player on a pad, whose cancel button is
the same rule - was put on the main menu instead of back where they were.

**The results of a server's race** had a button saying "Back to the lobby" and
an Escape that went to the main menu. One screen, two ways out, two different
places, and the one a player reaches for by reflex was the one that left the
room. Recoverable - PLAY on the title takes an online player back to the lobby -
but not what the screen said.

Both are now one rule: `gs_records_back` is written once and called by both the
button and `gs_menu_back`, and the results screen backs out where its own button
points. The switch names **every screen and has no `default`**, so a screen added
next year has to have its way out chosen rather than inheriting one - the same
`-Wswitch` guarantee the surfaces got this morning, for the same reason.

### The test walked nine of thirty-six, and one of them lied

`there_is_always_a_way_back_out_of_wherever_you_are` set `m.online = true` to
check the race case and then ran six more screens through a loop **without
setting it back**. So those six were only ever asked what Escape does *on a
server*, and the answer off one was never asked at all. That is how the results
screen kept its two different exits: the case that was wrong was the case
nothing looked at.

It walks every screen on a server and off one now, with the construction set
open and shut - 18 ways out - and the records screen from all nine screens as
the place it came from, including the ones nobody can arrive from, because the
safe answer for those is part of the claim. The table also asserts that every
screen appears in it exactly once, so a tenth screen turns the tree red by
itself.

## Rebinding a control from the keyboard could only ever bind Space

The coverage build put `gs_capture_rebind` at **a third of its 36 lines**. It
could not be tested where it was: it read SDL's live keyboard and a live pad,
and no test has either. What that was hiding is a fault in the middle of a
feature `gs_bind.h` calls "not a luxury feature here" - four people on one sofa,
a left-handed player, somebody who cannot reach the default keys.

**A capture begins the instant the control is pressed, and that control was
pressed with something.** Space or Enter, if the player walked to it with the
keyboard. The pad's bottom button, if they walked to it with a pad. That key is
still down on the very next frame, when the capture reads the keyboard for the
first time - so the action was bound to it immediately, before the player had
touched the key they meant.

Which means rebinding from the keyboard could only ever produce Space, and
rebinding from a pad could only ever produce the button that pad presses
everything with. Those two are most of the people the feature exists for. A
player with a mouse never saw it.

### Where the rule went, and why there

`gs_bind_pick` sits in `gs_bind.c`, beside `gs_bind_resolve`, which is the other
half of the same idea and was written this way on purpose: *the resolution is a
pure function of "which keys are down" and "which pad buttons are down", so it
can be tested without a keyboard or a pad.* `gs_bind.c` is at 98% coverage. The
capture was the same kind of decision left in the half that talks to SDL, and it
was at 33%.

It takes an `armed` flag the caller keeps for the duration of the capture:
nothing is accepted until everything has been let go once. Escape included -
cancelling on an Escape that is only still held from starting the capture would
cancel every rebind a keyboard player ever began.

### Every key, not a handful of interesting ones

**510 of 510 scancodes** can be bound to, and **all 26 pad buttons**. Not a
sample: what a player reaches for is theirs to choose, and a scancode that
cannot be captured is a control somebody cannot have. Escape is the single
exception and is the documented one - it means leave the binding alone.

Also pinned: the keyboard wins over a pad held at the same moment, nothing held
decides nothing however long it goes on, and a null keyboard - what a caller
gets before SDL has one - is not a crash.

## A circuit and a sprint over the same ground were one track

Walking every kind of edit through undo turned up one that "changed nothing":
switching a track between a loop and a path. It changed the track, so what it
had not changed was the **hash** - and the hash is what a track *is*.

The comment over `gs_track_hash` says the route is part of a track's identity,
*"the same ground driven the other way round is a different track, and its times
are not comparable"*. It hashed the gates. It did not hash whether those gates
make a loop or a path, which is the most literal reading of that sentence: on a
circuit you cross gate zero again to finish a lap; on a sprint you drive from
the first gate to the last and stop.

**The library is content addressed**, and it says so: *"the same track twice is
one track"*. So:

> Build a lap. Save it. Turn it into a run and save that under a second name.
> You have one track afterwards, with the second name on the first track.

Somebody's work, gone, silently. And records keyed on the same number pooled a
lap of a loop with a run from end to end - two times that cannot be put beside
each other, which is the entire reason a track has an identity.

`a_loop_and_a_path_over_the_same_ground_are_two_tracks` is the fault written
down: it failed before the fix with the library folded into one entry, and names
the entry it found there.

### What it cost to move, and what it did not

**The world hash did not move**, nor did the opponents race. Nothing about the
physics changed; what changed is what counts as the same track.

`GS_SELFTEST_TRACK_HASH` moved, deliberately. So did
`GS_SELFTEST_GENERATOR_HASH`, and that one names nothing: it is a fold of
`gs_track_hash` over the first two hundred seeds, so it moves when the function
does. **Every seed builds exactly the same ground it built before.**

A share code carries this hash so a damaged code fails loudly - and codes went
out with `v0.1.0-beta1` eight days ago. Changing the identity would have told
those people their working codes were damaged. `gs_track_hash_before_route_kind`
answers what the function used to, the reader accepts either, and the version
two code pinned in the test still opens. What that gives up is noticing a code
whose *route byte alone* was corrupted - one bit of one byte in a hundred, which
would still open as a real track - and the alternative was worse.

### And the shipped tracks were two short

Regenerating `assets/server/gearstick.db` meant re-running the chooser, and it
wrote **two tracks that are not in the repository**. Not because of anything
here: which generated tracks ship is decided by *racing* them - every vehicle
has to be able to finish, or a stock track tells a new player their choice of
machine was wrong - and the simulation has changed several times since those
files were committed. Two seeds became finishable and nothing re-ran the
choosing.

`tables.yml` exists precisely to keep committed generated artefacts honest, and
it did not fire: its `paths` list covers the generators and the assets, and not
the simulation that decides what the generators produce. `src/core/**` is in
that list now.

### And a guard for the next field, not just this one

A test naming the loop-and-path case catches the loop-and-path case. The general
fault is that a field was added to `gs_track` and the function that says what a
track *is* was not told, and nothing anywhere could notice.

`a_track_is_identified_by_everything_that_is_on_it` flips **every byte of a
track** and requires the identity to move exactly when the hash claims to read
that byte: 315 bytes are what a track is, 16842 are room the arrays have and
nobody filled. `w`, `h` and `gate_count` are counted apart and changed to other
legal values rather than flipped, because a flipped one says 255 tiles and sends
the hash reading off the end of a 64-tile array - a test crashing, not a fault
found.

So a field added next year is either read by the hash or is named in that test
as deliberately not part of what a track is. It cannot be neither, which is what
`route` was for four commits.

## And the same sweep on the thing determinism is for

If a field can be added to a track without the function that says what a track
*is* being told, the same can happen to a **world** - and `gs_world_hash` is
what two machines compare to find out they have stopped agreeing. A field
missing from it is a disagreement neither machine can see, which is the failure
mode CLAUDE.md describes: agree for ninety seconds, then differ by a car length.

`a_race_is_identified_by_everything_that_decides_it` flips every byte of a raced
world. **8325 of 8848 bytes decide the race.** The rest are padding, cars and
hazards nobody added, and one field.

### The one field, named with its reason

`green_tick` - when the lights go green - is not in the hash. It gates whether
any input reaches a car at all, so it is unambiguously state that decides the
race.

It is *not* added, and that is a decision rather than an oversight. That number
is written into every networked recording as the state the peers agreed the race
ended in, and `gs_verify` re-races a log and rejects it if it does not arrive
there. Moving it would reject every recording made with `v0.1.0-beta1` as a
different race - and unlike the track hash, this one cannot take "or what it
used to be" for an answer without weakening the one check that says a claimed
time is real.

What covers it is now **asserted rather than assumed**: while the lights are red
every car's lap clock is pinned to the green, so two worlds that disagree about
when it comes disagree in the hash through that. Take the pinning away and the
test goes red, and whoever does that has to hash `green_tick` instead. That is
the difference between a gap somebody noticed once and a gap that is held shut.

### A car is 56 bytes and the hash reads 52 of them

Padding cannot be located portably - a compiler puts it where it likes, and this
builds under four of them - so what is checked is the arithmetic instead. A
car's hashed fields add up to a number, the struct is a number, and the
difference is how many bytes inside a car somebody is driving may sit still.
**Add a field to `gs_car` and that difference grows and the test fails**,
whatever the offsets happen to be on the machine it is built on.

## Four things to leave behind, and a button that can choose

**The mine was written, hashed, tested for its effect, and no player could ever
drop one.** `GS_IN_FIRE` had exactly one reader in the simulation and it was
hard-coded to oil. The plan item *Droppable hazards* was ticked and its
verification describes the mine going off - which is true of the code and was
not true of the game. Found by listing every count sentinel in the tree and
asking which are walked by a test: `GS_HAZ_COUNT` was in none.

The toolkit is four now.

| | what it does | how it ends |
| --- | --- | --- |
| oil | takes the grip away and gives it back when you leave | stays |
| mine | one use: launches and hurts whoever found it | being found |
| smoke | hides the ground under it, and nothing else | eight seconds |
| fire | burns while you are in it | five seconds |

Fire is the one worth reading twice: **a mine punishes arriving and fire
punishes staying**, which is why it is not spent by being found. Smoke is the
one that is not physics at all - a car drives through it exactly as it would
have, and what it changes is what the driver behind can see.

### One button, and half a second decides which thing it does

Four people share one keyboard, so a fifth key each is a key somebody has to
reach. **A tap drops what is selected; holding for half a second moves the
selection on and drops nothing.** The drop lands on the *release*, because at
the moment of the press nothing yet knows whether this is a tap or the beginning
of a hold.

It is the same control on a pad as on a keyboard without any extra work, because
the binding for fire already carries a key *and* a pad button per player and
`gs_input_poll` ors them together.

Spending the last of something moves the selection to whatever is left, so the
button keeps doing something rather than going dead in the hand; running out of
everything selects nothing, so it cannot look armed when it is not.

### Ammunition is world state

Each car carries a count per kind, set before the flag and spent during the
race. That is hashed, like everything else that decides a race: two machines
disagreeing about how many mines somebody has left is a disagreement about what
is about to happen.

**A race with the weapons turned off is every car carrying zero of everything**,
which is what makes the setting a setting rather than a branch in the
simulation - and is why every race that came before behaves identically.

### The hashes that moved, and the one that did not

`GS_SELFTEST_WORLD_HASH` and `GS_OPPONENTS_WORLD_HASH` both moved. Nothing about
those two races changed - the cars end up in the same places - but a car carries
more state than it did, so the number describing it is longer. The **track**
hash did not move.

`a_race_is_identified_by_everything_that_decides_it`, written a few hours
earlier, failed the moment the fields went in: *"a car is 68 bytes and the hash
reads 52 of them"*. That is precisely the job it was written for, and it caught
its author.

### The switch, and where it had to go

The loadout is a race setting now - it lives on `gs_world` beside the gravity
and the lap target, for the same reasons: a replay rebuilds a world from the
settings and has to arm it the same way, and a record has to be filed under it.
Every car on the grid gets the same, **including one added after the loadout was
set**, so it cannot depend on the order the setup screen happens to build a race
in.

On the setup screen it is one line: a switch and four counts. **No heading of
its own, and that is the height budget talking** - the panel already fills a
720-tall screen at four drivers, and the rule that nothing sits below the fold
at the size the game opens at is worth more than a section title.

Paying for that line cost six pixels, and the first place they came from was the
box at the top that names the track. It holds two lines of text and looked like
it had room; it did not, and the second line came out with its descenders sliced
off. **Nothing could have caught that**: a child region clipping its own
contents is what a child region is for, so no panel measurement is looking. A
photograph was. The six pixels came from a gap above a separator instead, which
had them to give.

### A lap set among the oil is filed apart from a clean one

Weapons are conditions, like the gravity a time was set at. Folded into the
record key unconditionally they would have given every *weapons-off* race a new
key - and every best lap anybody has ever set would quietly stop being found on
their own records screen. So the loadout is folded in **only when there is
any**, and a race without them hashes exactly the way it always did.

### What is not done yet

- ~~A replay does not carry the loadout.~~ **Done.** Version six appends what
  the race armed everybody with, and the world is armed before anybody is placed
  because that is what puts ammunition on a car. A recording made before weapons
  reads as all zero, which is a race with none - which is what those races were.
  The version anybody actually has a file of is five, and there is a test that
  builds one and reads it.

  Verification gets it for free: `gs_verify` rebuilds the race through
  `gs_replay_restore`, so the conditions it recomputes now match the ones the
  client filed the claim under. Without this a perfectly honest claim from a
  weapons race would have been rejected for not arriving where it said.
- ~~No opponent has ever dropped anything.~~ **Done.** A driver leaves something
  behind when somebody is within seven tiles *and behind it*, which is the whole
  reason the weapon is worth having: you cannot shoot forwards, so hurting
  somebody means getting in front of them and staying there, and that is a race.

  **Once every forty ticks, not while the condition holds.** The button drops on
  release and changes the selection when held, so a driver that simply held it
  down would cycle through its weapons and never leave one - the shape of the
  control, not a detail of it.

  And the opponents hash did *not* move, which is better than the plan expected:
  carrying nothing is pressing nothing, so a race with the weapons off is
  bit-for-bit the race it was.
- ~~Nothing draws smoke or fire.~~ **Done.** The renderer knew two kinds - oil,
  and a small orange dot for everything else - so smoke and fire looked like
  mines, and smoke, whose entire job is hiding the ground, hid nothing. Four
  looks now, named one by one with no `default`, so a fifth kind has to be given
  one.

  And each is drawn **at the size the simulation will catch you at**, asked of
  `gs_hazard_radius` rather than guessed at in the renderer. A slick drawn
  narrower than it is is the kind of lie a player learns to distrust the whole
  physics over. Smoke and fire fade over their last third, so a driver can tell
  the fire they can wait out from the fire they cannot.

  The test measures by drawing the same ground **with and without** the hazard
  and comparing the same pixel. Comparing two different patches instead measures
  the terrain's own shading, which is how the test first told itself smoke was
  four tiles wider than it is.

## Weapons you can hear

`src/audio/` had never heard of a hazard. Four things a player can leave behind
and not one of them made a sound - and a mine you cannot hear behind you is a
mine that feels like the game cheating.

Hazards are not cars: there are up to thirty-two of them, they have no engines,
and what one makes is a single event rather than something that goes on for the
whole race. So they share a small bank of struck voices - **eight**, on purpose,
because eight things going off at once is already more than anybody can pick
apart and a hundred is mud. The quietest slot is taken when they are all busy,
so a mine going off is never lost to four slicks being poured.

| | what it sounds like |
| --- | --- |
| oil | poured: low, wet and over quickly |
| mine, laid | a click, and the quietest of the four - a mine you can hear being laid is a mine nobody drives over |
| mine, found | low, loud and long enough to turn round for |
| smoke | a hiss that goes on, because the canister is still emptying |
| fire, lit | a whoosh: something catching |
| fire, burning | a level rather than an event, chased rather than set, so it fades when it goes out |

**Noticed rather than reported.** The simulation is never asked to say "a mine
went off" - the mixer looks at what changed since last time, exactly the way an
impact is already found from a jump in a car's damage. Sound is downstream of
the simulation and never upstream, and that is what it costs.

A mine becoming spent is a bang; smoke and fire become spent by *burning out*,
which is not one, and the code says which is which rather than treating `spent`
as one thing.

### A third compiler difference, and this one was right

The colour switch initialised `col` in every case it had, and covered every
enumerator with no `default` - the guarantee that a fifth kind of hazard cannot
inherit the fourth's look. gcc and clang were happy. **MSVC was not, and MSVC
was correct**: `h->kind` is a `uint8_t` in the world, so it can hold a number
the switch has no case for, and `col` would then be used unset.

`col` starts at nothing and is skipped if it stays there. The `-Wswitch`
guarantee is untouched, because there is still no `default` for a new enumerator
to hide in. Worth writing down that the local clang build cannot catch this
class: MSVC's flow analysis is stricter than either compiler here, and the only
place that runs is CI.

## The HUD says what you are carrying

A hold that changes something invisible is not a control. The tap-and-hold is
explained once, on the setup screen, which is gone by the time anybody is
driving - so the screen has to say what a tap would leave and how many are left.

One small row, and **only when there is something to carry**: a race with the
weapons off does not get a row that says nothing, because the panel is sized
from the rows it has and a row that means something in one race and nothing in
every other is a hole in all the others. It goes when the last one is spent
rather than sitting there saying zero.

It names each kind with `gs_hazard_name`, the same call the setup screen uses,
so the screen you choose on and the screen you race on cannot drift apart.

### Read back the way the HUD already reports itself

The item probe names the widgets a person *presses*; the HUD is plain text, so
what it says cannot be read from the hooks at all. `gs_hud_carrying` reports the
row's text the same way `gs_hud_overflow` and `gs_hud_spare` already report what
did not fit and what was left over - which is the established shape here, and it
is why those two exist.

### Twelve states became twenty-four

Carrying something adds a row, so every HUD state is measured with weapons and
without. Walked as a dimension rather than as six more hand-written states: it
is independent of every other flag, and hand-picking combinations is how a state
goes unmeasured. **432 panels** across two, three and four players at two window
sizes, each inside its own view with no hole in it.

It found one immediately. The row cost a gap too much in exactly the three
states where somebody is waiting - the last row on a panel pays for a gap the
others do not, because ImGui's content ends at the last item rather than after
the spacing that would follow it. Measured against `gs_hud_spare` rather than
derived, which is what that number is for.

## Three players, and a quarter of the screen doing nothing

The one open question in `FEATURES.md` that was a defect rather than a choice:
*"what the merged four-player camera does when it cannot merge. The failure
mode is the design, and it has not been thought about yet."*

What it did was give three players the four-player grid and stop after three
panes. **A quarter of the window - 230,400 pixels at 1280x720 - blank for the
whole race**, while the three people racing were each squeezed into a box a
quarter the size. Nobody chose that; it fell out of a loop that stops at
`views`.

The decision, as three rules:

- **every pane is the same size**, because an unequal pane is an advantage and
  this is a game people play on one sofa;
- **the panes fill the screen**, apart from the divider between them;
- **no pane overlaps another**, or two players are looking at the same pixels
  and one of them is wrong.

Three columns rather than rows, to match the two-player split: going from two
players to three then changes how many panes there are and not which way they
run. The last column takes the remainder so the three tile exactly.

### The rule it replaced, and why that was the wrong trade

`four_players_get_four_views_that_tile_the_window_without_overlapping` said it
outright: *"Three and four take the same grid: a player joining should not
rearrange everybody else's screen."* That sounds right and is not.

**A player does not join a race.** The grid is settled on the setup screen or in
the lobby before the flag, and a machine that leaves a race in progress goes
back to the lobby rather than into one. So the rearrangement being avoided
happens between races, where it costs nothing - and it was being paid for with a
blank quarter for the whole of every three-player race.

The reasoning is in the test rather than in a commit message, because the next
person to read that test is the one who needs it.

### What was considered and not taken

Panes that hold whichever cars happen to be *together* - three cars in one pane
and the fourth in its own, changing as the race spreads and closes. It uses the
screen better still, and it makes the shape of the screen something a player
cannot predict: two panes, then three, then two. Predictability is the whole
ethic, and it is written down in `FEATURES.md` beside the decision rather than
left to be re-proposed.

## The server was not 42% tested; it was 42% *measurable*

The same trap as `main.c`, and I walked into it a second time. `test_server.c`
ended every one of its twenty-six tests with `SDL_KillProcess(server, true)` -
force, SIGKILL - and a process killed that way flushes nothing for a coverage
build to read. Asked to stop instead, the server's own loop measures **83% of
its lines and 24 of its 25 functions**.

The lesson is not about the server. It is that **a coverage number over a
subprocess is a statement about how the subprocess was stopped**, and there is
no warning when it is wrong - the number is simply low, and low looks like work
to do.

### What the killing was hiding

**The shutdown path had never run.** The server catches `SIGINT` and `SIGTERM`
and sets a flag its loop reads, and every test shot it before it could get
there. A server that had stopped stopping would have hung forever on somebody's
machine and nothing here would have noticed. It stops in **11 ms** when asked,
and the test fails if it ever has to be forced.

What that test proves depends on the platform, and it says so rather than
hiding it: where there are signals, asking is `SIGTERM` and this is the handler
working; on Windows, asking is `TerminateProcess` and all it shows is that the
process ends.

**The dashboard had never been drawn by anything.** Fifty-nine lines - the
server's only user interface - at zero, while twenty-six tests hammered it.
Drawing it needs a terminal, and every test and check gives it a pipe. It has
one now: `server_output_check.py` opens a pty and requires the dashboard to
appear *and to repaint* - it drew thirteen times in three seconds. Where there
is no pty the check says so rather than passing quietly.

That check's original half is untouched and still means what it said. It pins
what the server does when its output is **not** a terminal, which is the fault a
player found: a dashboard repainting into a pipe nobody drains fills it, and a
server blocked in `printf` answers nothing. The two halves are the two sides of
the same gate.

**`--help` had never run either** - twenty lines listing every flag the server
takes, which nothing would notice going stale except somebody typing it and
being told about a flag that no longer exists. The test names all five and
requires the process to *exit*, because a server that printed its usage and then
bound a port would start every time somebody asked it a question.

### What is left, named

`gs_heartbeat`: eight lines, the once-a-minute line that says a server with no
terminal is still alive. No test runs for a minute, and making one that does to
cover eight lines is a worse trade than saying it is not covered.

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
  triangles sort by painter's order alone. This has now been hit and answered
  once: the risk is not near-convexity but *interpenetration*, which a
  painter's sort cannot order at all, and the answer was to stop generating
  interpenetrating geometry rather than to sort it better - see "Cars that are
  drawn whole" above. A single closed surface sorts acceptably. When that stops
  being true, `SDL_GPU` is the swap the thin `src/gfx/` interface exists to
  allow.

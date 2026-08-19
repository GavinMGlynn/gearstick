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

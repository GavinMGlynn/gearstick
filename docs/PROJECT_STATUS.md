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

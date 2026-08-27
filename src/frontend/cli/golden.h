// golden.h - the state hash the selftest race is required to end on.
//
// **This number changing is not a test being annoying.** It means the physics
// moved, and every ghost time, every shared replay and every track anyone has
// tuned against is now wrong. Updating it is a deliberate act with a note in
// docs/PROJECT_STATUS.md saying what changed and why it was worth it.
//
// Print the current value with:  gearstick_cli selftest
#ifndef GS_GOLDEN_H
#define GS_GOLDEN_H

// Moved once, deliberately, when gates were added: a track's identity now
// includes its route, because the same ground driven the other way round is a
// different track and its times are not comparable. The *world* hash below did
// not move, which is the point - the physics was untouched, and that is exactly
// what these two numbers being separate is for.
//
// **And a second time, for the other half of that same sentence.** The hash
// covered the gates and not whether they make a loop or a path - so a circuit
// and a sprint over exactly the same ground were one track. The library is
// content addressed, so saving one beside the other renamed the first entry and
// threw the second away: somebody's work, gone, silently. Records keyed on this
// pooled a lap of a loop with a run from end to end, which is two times that
// cannot be put next to each other.
//
// The world hash did not move again. Nothing about the physics changed; what
// changed is what counts as the same track.
//
// A share code carries this number so a damaged code fails loudly, and one went
// out with v0.1.0-beta1, so those codes would have stopped opening. They do not:
// the reader accepts the answer this used to give as well. See
// gs_track_hash_before_route_kind.
#define GS_SELFTEST_TRACK_HASH 0x483dd875662890aeULL
// Moved twice more since, both times deliberately and both times because the
// race really did change: the grip circle, and then the roster retune that
// followed from it. A vehicle's numbers are inputs to the physics, so changing
// them changes every recorded replay - which is why this number is allowed to
// move during tuning and is not allowed to move by accident.
//
// And again when laps gained a clock: a best lap is what a track is judged by,
// so it is simulation state rather than something a front end watches for.
//
// And when a race gained a finish line: a lap target on the world and a finish
// tick on each car, both hashed, because a results screen that shows times
// needs the times to be part of the state two machines have to agree about.
//
// And when ground too steep to climb became a wall rather than a catapult -
// found by the analyser calling an impassable track completable.
//
// And again for car-to-car collision, which cars in the selftest race now have
// with each other.
//
// And once more for surface wear, which is new state that changes the race:
// dirt churns and loses grip on the line everyone takes, ice polishes into
// something faster and looser, pavement is unmoved.
//
// The grip circle, in detail: engine force is now
// capped by the traction available, so tyres matter off the line and not only
// in corners, and low gravity takes your acceleration along with your weight.
// Every replay and ghost time recorded before that is invalid, which is what
// this number failing is for. See docs/PROJECT_STATUS.md.
// And once more when the world stopped going on forever. There is now a run-off
// outside a track and a drop past it, so the ground a car finds off the edge is
// sand at the edge's height for ten tiles and then falling away - where before
// it was the edge tile's own surface, level, without end. The selftest race has
// a car that leaves the track, so its hash moves. See docs/PROJECT_STATUS.md.
#define GS_SELFTEST_WORLD_HASH 0xd831fad1fd238c26ULL

// The track generator, folded over its first two hundred seeds.
//
// **A generated track is identified by its seed**, so two people typing the same
// number have to get the same ground - across compilers and across platforms,
// exactly like the physics. This number caught the reason it needs to exist: two
// RNG draws sitting in one argument list are two draws in an order C does not
// define, and gcc and clang chose differently. Changing the generator's output
// deliberately means moving this number and saying so; changing it by accident
// means somebody's shared seed no longer names the track they meant.
// Moved once, deliberately, when the surfaces went from three to nine: the
// generator picks the ground it builds on, so six more grounds is six times as
// many things it can pick and every seed lands somewhere new. Nobody had shared
// a seed yet. The *world* hash below did not move, because the three original
// surfaces kept their numbers and their physics - which is what appending to
// that enum rather than renumbering it is for.
//
// Moved a second time, deliberately, when the tracks became raceable. The
// generator used to lay two gates on every shape it built - one near the left
// edge, one near the right - and the simulation counted a lap when the last
// gate was crossed, so a "lap" was a one-way trip and lap two meant driving
// back across an open field with nothing marking the way. A player put it
// plainly: the track does not seem to go anywhere.
//
// Every seed now builds either a loop or a path, the route is *carved* into the
// ground rather than dropped on top of it, and the road has its own surface so
// it can be seen. Every seed therefore lands somewhere new, and every seed
// anybody had shared names a different track. That is the cost and it was worth
// paying: what the old seeds named was not raceable.
//
// Moved a third time, in the same breath, because the tracks that came out of
// the second attempt still could not be finished. A gate is finite across its
// line, and the generator was laying gates narrower than the road it had just
// carved - so a car keeping to the outside of its own road went *past* a
// checkpoint without crossing it, and because gates count in order the finish
// line then did nothing when it was reached. A player drove over the finish and
// the game did not notice. Gates are wider than the road now, and
// `every_gate_is_wider_than_the_road_it_crosses` keeps them that way.
//
// The *world* hash below did not move. The physics is untouched by any of this.
//
// **Moved a fourth time, and this one names nothing.** This number is a fold of
// `gs_track_hash` over the first two hundred seeds, and that function changed -
// a track's identity now says whether it is a loop or a path. The generator did
// not change. **Every seed builds exactly the same ground it built before**; the
// number that names that ground is different, which is what the fold measures.
// So unlike the three moves above, nobody's shared seed opens a different track,
// and the warning this failure prints is worth reading with that in mind.
#define GS_SELFTEST_GENERATOR_HASH 0x84b090da5b530ccfULL

// **A race with nobody at the keyboard**, four opponents spread across the
// skill dial, on a circuit none of them has seen.
//
// This one moves for two reasons rather than one: the physics, like the number
// above it, and *the driver*. An opponent is a pure function of the world, so a
// change to how it decides where to lift or which way round a wall to go is a
// change to every race anybody has recorded against it - and unlike a scripted
// input log, there is nothing else holding the race in place. That is exactly
// why it is pinned.
#define GS_OPPONENTS_WORLD_HASH 0x7e8fe47bf3be48b4ULL

#endif // GS_GOLDEN_H

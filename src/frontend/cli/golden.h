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
#define GS_SELFTEST_TRACK_HASH 0x254cc5e1aae6c99aULL
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
// The *world* hash below did not move. The physics is untouched by any of this.
#define GS_SELFTEST_GENERATOR_HASH 0x7b1a7fb8d486b941ULL

#endif // GS_GOLDEN_H

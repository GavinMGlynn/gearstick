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
#define GS_SELFTEST_WORLD_HASH 0x38f969eb07c8e74aULL

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
#define GS_SELFTEST_GENERATOR_HASH 0xab0105176f872f72ULL

#endif // GS_GOLDEN_H

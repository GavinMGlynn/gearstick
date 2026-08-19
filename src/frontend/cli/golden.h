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
#define GS_SELFTEST_WORLD_HASH 0xfdecd61921f4583dULL

#endif // GS_GOLDEN_H

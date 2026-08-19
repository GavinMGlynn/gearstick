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
// Moved once, deliberately, when the grip circle arrived: engine force is now
// capped by the traction available, so tyres matter off the line and not only
// in corners, and low gravity takes your acceleration along with your weight.
// Every replay and ghost time recorded before that is invalid, which is what
// this number failing is for. See docs/PROJECT_STATUS.md.
#define GS_SELFTEST_WORLD_HASH 0x28b4a976d838f140ULL

#endif // GS_GOLDEN_H

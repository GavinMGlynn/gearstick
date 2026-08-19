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

#define GS_SELFTEST_TRACK_HASH 0x45680cd170357efeULL
#define GS_SELFTEST_WORLD_HASH 0x1efc0b9d2feb87b1ULL

#endif // GS_GOLDEN_H

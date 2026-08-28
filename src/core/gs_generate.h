// gs_generate.h - tracks from a number.
//
// **Seeded, so a track is reproducible from a number.** That is not a
// convenience: it is the same property the rest of this project is built on.
// A generated track can be named by its seed, regenerated anywhere, and shared
// as four bytes - and because the result is content-hashed like any other
// track, two people generating seed 41 have provably the same ground.
//
// The generator is deliberately not clever. It picks a shape, lays a route
// through it, and decorates - because a track somebody can predict the shape of
// is a track they can learn, and the whole ethic here is a model simple enough
// to exploit. A generator that produced surprising terrain would produce tracks
// nobody could read.
//
// Integers only, like everything in src/core/: the same seed has to give the
// same track on every machine, and a float would make that a hope.
#ifndef GS_GENERATE_H
#define GS_GENERATE_H

#include "core/gs_track.h"

typedef enum gs_track_shape {
    GS_SHAPE_SPRINT = 0,   // out and back, the shape a first track should be
    GS_SHAPE_CIRCUIT,      // a loop with corners
    GS_SHAPE_JUMPS,        // a run of ridges, where the air time is the point
    GS_SHAPE_MIXED,        // surfaces that change under you
    GS_SHAPE_COUNT
} gs_track_shape;

// Half the width of the road a generated route is carved along, in tiles.
//
// Public because a gate has to be wider than it. A gate is finite across its
// line - that is what makes it a gate rather than a tripwire across the world -
// so one narrower than the road can be driven *past* on the outside, and
// because gates count in order a checkpoint nobody crossed is a finish line
// that never fires. A player drove over the finish and the game did not notice.
#define GS_GEN_ROAD 4

const char *gs_shape_name(gs_track_shape s);

// Build one. The seed decides everything - the shape, the size, the terrain,
// the surfaces, the painted gravity and the route - so the same seed gives the
// same track, byte for byte, on any machine.
void gs_generate(gs_track *t, uint32_t seed);

// The same, with the shape chosen rather than drawn from the seed.
void gs_generate_shape(gs_track *t, uint32_t seed, gs_track_shape shape);

// **The route, laid onto ground somebody else built.**
//
// A serpentine of the same shape the generator uses, carved into whatever
// terrain is already there, with its gates. It is public because the tracks
// written by hand in tools/make_tracks.c need to be as long as the generated
// ones - each of them demonstrates one idea, and a route that crosses that idea
// five times is a better demonstration than one that crosses it once.
void gs_generate_route(gs_track *t, uint32_t seed, bool loop);

// Which shape a seed gives, without building it - for a sweep that wants to
// report what failed rather than merely that something did.
gs_track_shape gs_generate_shape_for(uint32_t seed);

// A name for a generated track, from its seed. Deterministic, so the same seed
// is the same name - two words, because "seed 2864434397" is not something
// anybody repeats out loud.
void gs_generate_name(char *out, size_t cap, uint32_t seed);

#endif // GS_GENERATE_H

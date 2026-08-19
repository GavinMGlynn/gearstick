// gs_stunts.h - reading a track out of Stunts (1990).
//
// **Because every track this project has ever seen was built by whoever wrote
// the editor.** That is the worst possible sample: the shapes that get built are
// the shapes the tools make easy, and the analyser, the AI and the renderer have
// only ever been shown those. A generated track does not help - the generator
// was written by the same person, from the same assumptions, and it produces
// exactly the terrain it was told to.
//
// Stunts is a good donor. It is grid based, it has elevation, and it has three
// road surfaces - paved, dirt, ice - which happen to be the three this project
// started with. Its format is documented and small: 1802 bytes, a 30x30 grid,
// one plane of road pieces and one of terrain.
//
// **Two separate questions, and they get separate answers.** Reading the format
// is our code and it ships. The tracks themselves are somebody else's and do not
// ship, whatever their licence permits - `docs/ASSETS.md` rule 1 says nothing
// third-party is redistributed by this repository, and a track corpus is no
// different from a sprite sheet. What ships is the reader; what is downloaded is
// the reader's input, by whoever wants to use it.
//
// Links nothing, like the rest of src/core/: it takes bytes and produces a
// gs_track. Where the bytes came from is the caller's business.
#ifndef GS_STUNTS_H
#define GS_STUNTS_H

#include "core/gs_track.h"

// A Stunts track is exactly this big, and exactly this shape.
#define GS_STUNTS_BYTES 1802
#define GS_STUNTS_SIDE  30

// Where each part of the file is. Straight from the format, and written down
// here because the two 900-byte planes are indistinguishable by inspection and
// getting them the wrong way round produces a track that looks plausible.
#define GS_STUNTS_TRACK_AT   0      // 900 bytes: road pieces
#define GS_STUNTS_HORIZON_AT 900    // 1 byte: which scenery
#define GS_STUNTS_TERRAIN_AT 901    // 900 bytes: ground

// What the file said the scenery was. Not used by the conversion - we have our
// own grounds - but reported, because it is the one piece of authorial intent in
// the file that is not geometry.
typedef enum gs_stunts_horizon {
    GS_STUNTS_DESERT = 0,
    GS_STUNTS_TROPICAL,
    GS_STUNTS_ALPINE,
    GS_STUNTS_CITY,
    GS_STUNTS_COUNTRY,
    GS_STUNTS_CHAOTIC
} gs_stunts_horizon;

typedef struct gs_stunts_report {
    bool     ok;
    uint8_t  horizon;
    uint16_t road_tiles;      // how many squares had a road piece on them
    uint16_t raised_tiles;    // how many had something other than flat ground
    // Squares holding something this reader could not name - a loop, a pipe, a
    // bridge, or a code the table it was written from does not list. They are
    // laid as road, because a car should be able to drive where the donor put
    // one, and counted, because "it imported" means nothing without knowing how
    // much of it was approximated.
    uint16_t unknown_pieces;
} gs_stunts_report;

// Convert. `out` is filled with a track 30 tiles square whatever happens; the
// report says how much of the file was understood.
//
// **False only when the bytes are not a Stunts track at all** - the wrong length
// is the only thing that can be said for certain, because every byte value in
// range is a legal picture of something. A file full of pieces this reader does
// not know still converts, and says so in `unknown_pieces`, because half a track
// somebody can look at beats a refusal they cannot act on.
bool gs_stunts_read(gs_track *out, const uint8_t *bytes, size_t len,
                    gs_stunts_report *report);

// The reverse, for tests: write a track back out in the Stunts layout. **Not a
// general exporter** - it only writes what the reader understands, and it exists
// so that the reader can be exercised in CI against a file this repository made
// rather than against one it downloaded.
size_t gs_stunts_write(const gs_track *t, uint8_t *buf, size_t cap,
                       uint8_t horizon);

#endif // GS_STUNTS_H

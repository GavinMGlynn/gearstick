// gs_music.h - a theme per track, composed rather than recorded.
//
// **The seed is the track's own content hash.** A track already has an identity
// - the hash that says two people who built the same thing built the same thing
// - so that number is handed to the composer and every track gets its own tune.
// Build a ramp, get a different chorus. Undo it, get the old one back. Nobody
// has to write fifty pieces of music for fifty tracks, and nobody has to listen
// to the same one on all of them.
//
// The instruments are a 1985 machine's: pulse waves with a moving duty cycle, a
// triangle, and noise for percussion. That is not nostalgia for its own sake -
// three simple oscillators are what a generative score can actually be *good*
// at, because the parts that carry a chiptune are the composition and the
// arpeggios rather than the timbre.
//
// Deterministic, and tested for it: the same seed is the same music, sample for
// sample, on every machine. That is not required for the simulation - music is
// downstream of everything - but a tune that came out different each time would
// be a tune nobody could describe to anybody else.
#ifndef GS_MUSIC_H
#define GS_MUSIC_H

#include "core/gs_common.h"

// Compose and start playing. The same seed always gives the same piece.
void gs_music_start(uint64_t seed);

// Fade out and stop. Ramped, because a tune that stops mid-note is a click.
void gs_music_stop(void);

bool gs_music_playing(void);

// How loud, against the race. Music under a race wants to be well under it.
void  gs_music_set_volume(float v);
float gs_music_volume(void);

// Add this many frames of music into an interleaved stereo buffer. Additive:
// the race is already in there.
void gs_music_mix(float *out, int frames);

// What is playing, for a corner of the screen and for the tests: the key, the
// tempo, and how far through the piece it is.
typedef struct gs_music_state {
    uint8_t  root;        // semitones above A
    bool     minor;
    uint16_t bpm;
    uint32_t bar;
    uint8_t  chord;       // which degree of the scale this bar sits on
    uint8_t  step;        // sixteenth within the bar
} gs_music_state;

gs_music_state gs_music_now(void);

#endif // GS_MUSIC_H

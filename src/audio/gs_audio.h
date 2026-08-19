// gs_audio.h - the noise a race makes, synthesised rather than sampled.
//
// **Generated, for the same reason the art is.** There is no third-party audio
// in the game and nothing to license, and more usefully: a sample is a
// recording of one engine at one speed, and what this needs is an engine note
// that follows a drivetrain continuously from idle to the limiter and back down
// through a gearchange. That is a synthesiser's job, not a sampler's.
//
// Four things make the noise:
//
//   the engine    a harmonic stack whose frequency follows crankshaft speed,
//                 which follows road speed through a gearbox - so the note
//                 climbs, drops on the change, and climbs again
//   the tyres     filtered noise, loud when a car is sliding and coloured by
//                 what it is sliding on
//   impacts       a struck transient, from collisions and from landings hard
//                 enough to hurt
//   silence       a car in the air has no tyre noise at all, and the engine
//                 goes light because there is nothing loading it. This is the
//                 one everybody remembers.
//
// None of this is in src/core/. Sound is downstream of the simulation and never
// upstream: nothing here can change where a car ends up, which is why a race
// with the audio device missing is the same race.
#ifndef GS_AUDIO_H
#define GS_AUDIO_H

#include "core/gs_sim.h"

#define GS_AUDIO_RATE     48000
#define GS_AUDIO_CHANNELS 2

// Open the device. False if there is not one, which is not an error worth
// stopping for - the game is playable in silence.
bool gs_audio_open(void);
void gs_audio_close(void);
bool gs_audio_active(void);

// Tell the synthesiser what the world is doing. Called once a frame from the
// main thread; the audio thread reads a snapshot of it. `lx`, `ly` are where
// the listener is - the camera centre - so a car across the track is quieter
// than the one being driven.
void gs_audio_update(const gs_world *w, const gs_track *t, float lx, float ly);

// Everything off, for a menu or a pause. Ramped rather than cut, because an
// instant stop is a click.
void gs_audio_silence(void);

void  gs_audio_set_volume(float v);
float gs_audio_volume(void);

// Fill a buffer. This is what the device callback calls, and what the tests
// call - so what is measured is the same signal that comes out of the speakers
// rather than a description of it.
void gs_audio_render(float *out, int frames);

#endif // GS_AUDIO_H

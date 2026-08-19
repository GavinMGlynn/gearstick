// gs_share.h - a track as something you can paste into a chat window.
//
// The fifty-track floppy was a media limitation, not a design choice. A track
// here is a few kilobytes of mostly-flat ground, which packs to a few hundred
// bytes and prints as a few hundred characters of URL-safe text - so it leaves
// the room in a message, and arrives as the same track it left as.
//
// "The same track" is checked rather than hoped for. The code carries the
// track's content hash, and decoding rebuilds the track and compares: a code
// with a character missing is rejected instead of quietly becoming a different
// track that happens to parse.
#ifndef GS_SHARE_H
#define GS_SHARE_H

#include "core/gs_track.h"

#define GS_SHARE_PREFIX  "GST1"
#define GS_SHARE_SCHEME  "gearstick:track/"

// Comfortably past the worst case: a full 64x64 track with every corner
// different, packed, base64'd, plus the prefix.
#define GS_SHARE_MAX 32768

// Write the code for `t` as a NUL-terminated string. Returns its length, or 0
// if it would not fit.
size_t gs_track_to_code(const gs_track *t, char *out, size_t cap);

// Read one back. Accepts the bare code, and the same code as a URL, and either
// with surrounding whitespace - because what arrives is whatever the other
// person's chat client did to it. False if it is not a code, if it was damaged
// in transit, or if it does not rebuild the track it claims to be.
bool gs_track_from_code(gs_track *t, const char *code);

// The code wrapped as a URL, for the times a link is the thing that travels.
size_t gs_track_to_url(const gs_track *t, char *out, size_t cap);

#endif // GS_SHARE_H

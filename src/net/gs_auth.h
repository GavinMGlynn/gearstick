// gs_auth.h - a password, and a one-time code.
//
// **This is small because the tunnel came first.** Inside a sealed channel a
// password can simply be sent and a code can simply be quoted; before the
// tunnel both would have needed a challenge-response construction built for the
// purpose, which is exactly the kind of thing this project refuses to invent.
// The whole of the design is now "compare what they sent with what is stored",
// and the interesting part is entirely in what does the comparing.
//
// **Nothing here is invented either.** The password hash is libsodium's
// `crypto_pwhash_str` - Argon2id, with the salt and the cost parameters inside
// the string it returns, so there is nothing to store alongside it and nothing
// to get wrong. The one-time code is RFC 6238 over RFC 4226, on libsodium's
// HMAC.
//
// **The code is TOTP-SHA256, not TOTP-SHA1.** RFC 6238 names all three of
// SHA-1, SHA-256 and SHA-512, and libsodium ships the second and third and not
// the first. Choosing the one the audited library actually has beats
// implementing SHA-1 here to match what most phone apps default to - and it is
// said out loud rather than discovered by somebody whose authenticator app
// gives them six wrong digits.
#ifndef GS_AUTH_H
#define GS_AUTH_H

#include "core/gs_common.h"

// Long enough for crypto_pwhash_STRBYTES, which is 128.
#define GS_AUTH_HASH_BYTES 128
#define GS_AUTH_SECRET_BYTES 20      // RFC 4226 recommends at least 128 bits
#define GS_AUTH_STEP_SECONDS 30
#define GS_AUTH_DIGITS 6

// True on success. `out` gets a self-describing hash string; the password
// itself is never stored anywhere.
bool gs_auth_hash_password(const char *password, char *out, size_t cap);

// **Constant time, and it is libsodium's job.** Verifying a password by
// comparing strings is how long somebody's password is leaks; this hands the
// whole comparison to the library that owns the format.
bool gs_auth_check_password(const char *hash, const char *password);

// A fresh shared secret for a one-time code.
void gs_auth_new_secret(uint8_t *out, size_t len);

// The code for one time step. Separated from the clock so that a test can ask
// for a specific step rather than waiting for one.
uint32_t gs_auth_code_at(const uint8_t *secret, size_t len, int64_t step);

// The step a Unix time falls in.
int64_t gs_auth_step_of(int64_t now);

// Is this the code for a step within `slack` of now, and which step was it?
// **The window is the reason `slack` exists**: two clocks are never exactly
// together, and refusing a code because a phone is eleven seconds fast is a
// second factor nobody can use. The caller then has to spend that step, once -
// see `gs_store_totp_use` - because a window without a spend is a window in
// which a code works more than once.
bool gs_auth_check_code(const uint8_t *secret, size_t len, uint32_t code,
                        int64_t now, int slack, int64_t *step);

#endif // GS_AUTH_H

#include "net/gs_auth.h"

#include "core/gs_profile.h"

#include <sodium.h>
#include <string.h>

// **The two places these sizes are written must agree.** `src/core/gs_profile.h`
// carries a password hash and a TOTP secret as opaque bytes, and it cannot
// include this header to learn how big they are - core links nothing and
// including src/net/ from src/core/ is the layering violation the build checks
// for. So the numbers are written out there and pinned here, where both
// headers are in scope. A silent disagreement would truncate a hash on save.
static_assert(GS_PROFILE_PASSWORD == GS_AUTH_HASH_BYTES,
              "a profile's password field must hold a whole Argon2id hash");
static_assert(GS_PROFILE_TOTP == GS_AUTH_SECRET_BYTES,
              "a profile's TOTP field must hold a whole shared secret");
static_assert(GS_AUTH_HASH_BYTES >= crypto_pwhash_STRBYTES,
              "libsodium wants more room for an encoded hash than we reserved");

bool gs_auth_hash_password(const char *password, char *out, size_t cap) {
    if (password == nullptr || out == nullptr) return false;
    if (cap < crypto_pwhash_STRBYTES) return false;

    // The interactive parameters: about a tenth of a second and 64 MB on the
    // machine that made them. A racing server is not a password vault and the
    // moderate settings would make joining a game feel broken.
    return crypto_pwhash_str(out, password, strlen(password),
                             crypto_pwhash_OPSLIMIT_INTERACTIVE,
                             crypto_pwhash_MEMLIMIT_INTERACTIVE) == 0;
}

bool gs_auth_check_password(const char *hash, const char *password) {
    if (hash == nullptr || password == nullptr) return false;
    return crypto_pwhash_str_verify(hash, password, strlen(password)) == 0;
}

void gs_auth_new_secret(uint8_t *out, size_t len) {
    randombytes_buf(out, len);
}

int64_t gs_auth_step_of(int64_t now) {
    return now / GS_AUTH_STEP_SECONDS;
}

// RFC 4226 section 5.3, over RFC 6238's time-based counter. The counter is
// eight bytes big-endian - which is the one thing in this project that is not
// little-endian, and is that way because the specification says so.
uint32_t gs_auth_code_at(const uint8_t *secret, size_t len, int64_t step) {
    uint8_t counter[8];
    uint64_t c = (uint64_t)step;
    for (int i = 7; i >= 0; i--) {
        counter[i] = (uint8_t)(c & 0xffu);
        c >>= 8;
    }

    uint8_t mac[crypto_auth_hmacsha256_BYTES];
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, secret, len);
    crypto_auth_hmacsha256_update(&st, counter, sizeof counter);
    crypto_auth_hmacsha256_final(&st, mac);

    // Dynamic truncation: the low nibble of the last byte picks where to read
    // four bytes from, and the top bit of those is cleared so the result cannot
    // depend on how a signed integer is represented.
    size_t at = (size_t)(mac[sizeof mac - 1] & 0x0fu);
    uint32_t binary = ((uint32_t)(mac[at] & 0x7fu) << 24) |
                      ((uint32_t)mac[at + 1] << 16) |
                      ((uint32_t)mac[at + 2] << 8) |
                      ((uint32_t)mac[at + 3]);

    uint32_t modulus = 1;
    for (int i = 0; i < GS_AUTH_DIGITS; i++) modulus *= 10u;
    return binary % modulus;
}

bool gs_auth_check_code(const uint8_t *secret, size_t len, uint32_t code,
                        int64_t now, int slack, int64_t *step) {
    if (secret == nullptr || len == 0) return false;

    int64_t centre = gs_auth_step_of(now);

    // **Every candidate is tried even after one matches.** Stopping early would
    // make how long this takes depend on which step was right, and that is a
    // measurement of the code somebody just sent.
    bool found = false;
    int64_t which = 0;
    for (int i = -slack; i <= slack; i++) {
        int64_t candidate = centre + i;
        uint32_t want = gs_auth_code_at(secret, len, candidate);
        bool same = sodium_memcmp(&want, &code, sizeof code) == 0;
        if (same && !found) {
            found = true;
            which = candidate;
        }
    }
    if (found && step != nullptr) *step = which;
    return found;
}

// test_store.c - what the server remembers, and the rules it remembers it by.
//
// Every test runs against an in-memory database, so nothing here depends on a
// file left behind by the last run - which is the failure mode that makes a
// storage test pass on one machine and fail on a fresh checkout.
#include "core/gs_track.h"
#include "net/gs_auth.h"
#include "net/gs_store.h"

#include <sqlite3.h>   // to build a database this code did not make

#include <stdio.h>
#include <string.h>

static int gs_failures = 0;
static const char *gs_current = "";

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  FAIL %s\n    %s:%d: %s\n", gs_current, __FILE__,         \
                   __LINE__, #cond);                                           \
            gs_failures++;                                                     \
        }                                                                      \
    } while (0)

#define TEST(name)                                                             \
    static void name(void);                                                    \
    static void run_##name(void) { gs_current = #name; name(); }               \
    static void name(void)

TEST(a_store_opens_and_knows_what_it_is) {
    gs_store *s = gs_store_open(":memory:");
    CHECK(s != nullptr);
    if (s == nullptr) return;

    // Three, since drivers gained a password. The literal is deliberate: a schema
    // that changes without anybody meaning it to is a database somebody else
    // cannot open, so the number is written down and has to be edited on
    // purpose.
    CHECK(gs_store_version(s) == 3);
    CHECK(gs_store_driver_count(s) == 0);
    CHECK(gs_store_record_count(s) == 0);
    CHECK(gs_store_track_count(s) == 0);
    CHECK(gs_store_error(s) == nullptr);

    gs_store_close(s);
}

TEST(a_driver_is_remembered_once_however_often_they_appear) {
    gs_store *s = gs_store_open(":memory:");
    if (s == nullptr) { gs_failures++; return; }

    int64_t ada = gs_store_put_driver(s, "ada", 4, 2);
    CHECK(ada > 0);
    CHECK(gs_store_driver_count(s) == 1);

    // The same person again, with a repaint. One row, updated.
    int64_t again = gs_store_put_driver(s, "ada", 6, 3);
    CHECK(again == ada);
    CHECK(gs_store_driver_count(s) == 1);

    char name[GS_STORE_NAME];
    uint8_t colour = 0, vehicle = 0;
    CHECK(gs_store_driver(s, ada, name, sizeof name, &colour, &vehicle));
    CHECK(strcmp(name, "ada") == 0);
    CHECK(colour == 6);
    CHECK(vehicle == 3);

    CHECK(gs_store_put_driver(s, "bez", 1, 1) != ada);
    CHECK(gs_store_driver_count(s) == 2);

    // A nameless driver is not a driver.
    CHECK(gs_store_put_driver(s, "", 0, 0) == 0);
    CHECK(gs_store_put_driver(s, nullptr, 0, 0) == 0);
    CHECK(gs_store_driver_count(s) == 2);

    CHECK(gs_store_find_driver(s, "ada") == ada);
    CHECK(gs_store_find_driver(s, "nobody") == 0);

    gs_store_close(s);
}

TEST(a_record_is_a_time_on_a_track_under_conditions_over_a_distance) {
    gs_store *s = gs_store_open(":memory:");
    if (s == nullptr) { gs_failures++; return; }

    const uint64_t track = 0xabcdef0123456789ull;
    const uint64_t earth = 0x1111ull, moon = 0x2222ull;

    CHECK(gs_store_best_lap(s, track, earth, nullptr, 0) == 0);

    CHECK(gs_store_put_record(s, track, earth, 3, "ada", 2, 5000, 16000));
    char who[GS_STORE_NAME] = { 0 };
    CHECK(gs_store_best_lap(s, track, earth, who, sizeof who) == 5000);
    CHECK(strcmp(who, "ada") == 0);

    // **A lap at a sixth of gravity is not a lap.** Same track, different
    // dials, different table.
    CHECK(gs_store_best_lap(s, track, moon, nullptr, 0) == 0);

    // A different track, one bit apart, is a different table too.
    CHECK(gs_store_best_lap(s, track ^ 1u, earth, nullptr, 0) == 0);

    // Slower does not replace faster.
    CHECK(gs_store_put_record(s, track, earth, 3, "bez", 1, 5400, 17000));
    CHECK(gs_store_best_lap(s, track, earth, who, sizeof who) == 5000);
    CHECK(strcmp(who, "ada") == 0);

    // Faster does.
    CHECK(gs_store_put_record(s, track, earth, 3, "bez", 1, 4800, 17000));
    CHECK(gs_store_best_lap(s, track, earth, who, sizeof who) == 4800);
    CHECK(strcmp(who, "bez") == 0);

    // And somebody's own slower run never overwrites their own faster one.
    CHECK(gs_store_put_record(s, track, earth, 3, "bez", 1, 9999, 99999));
    CHECK(gs_store_best_lap(s, track, earth, nullptr, 0) == 4800);
    CHECK(gs_store_best_race(s, track, earth, 3, nullptr, 0) == 16000);

    // A race time only means anything against a race of the same length: a
    // five-lap run does not take the three-lap record, and does not lose it
    // either.
    CHECK(gs_store_put_record(s, track, earth, 5, "ada", 2, 4900, 26000));
    CHECK(gs_store_best_race(s, track, earth, 3, nullptr, 0) == 16000);
    CHECK(gs_store_best_race(s, track, earth, 5, nullptr, 0) == 26000);

    // The lap record is across every distance, because a lap is a lap.
    CHECK(gs_store_best_lap(s, track, earth, nullptr, 0) == 4800);

    // One row per driver per track per conditions per distance.
    CHECK(gs_store_record_count(s) == 3);

    gs_store_close(s);
}

TEST(a_track_is_stored_by_what_it_is) {
    gs_store *s = gs_store_open(":memory:");
    if (s == nullptr) { gs_failures++; return; }

    uint8_t bytes[512];
    for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(i * 7u);

    const uint64_t hash = 0x0f0f0f0f12345678ull;
    CHECK(!gs_store_has_track(s, hash));
    CHECK(gs_store_put_track(s, hash, "the loop", "ada", bytes, sizeof bytes));
    CHECK(gs_store_has_track(s, hash));
    CHECK(gs_store_track_count(s) == 1);

    // **The same track twice is one track.** Content-addressed, so two people
    // uploading the thing they both built collide correctly.
    CHECK(gs_store_put_track(s, hash, "the loop", "bez", bytes, sizeof bytes));
    CHECK(gs_store_track_count(s) == 1);

    uint8_t back[512];
    size_t len = 0;
    CHECK(gs_store_get_track(s, hash, back, sizeof back, &len));
    CHECK(len == sizeof bytes);
    CHECK(memcmp(back, bytes, sizeof bytes) == 0);

    // Asking about one that is not there says so rather than half-answering.
    CHECK(!gs_store_get_track(s, hash ^ 1u, back, sizeof back, &len));
    CHECK(len == 0);

    // A buffer too small is refused rather than written past - and still says
    // how big it needed to be.
    uint8_t small[16];
    len = 0;
    CHECK(!gs_store_get_track(s, hash, small, sizeof small, &len));
    CHECK(len == sizeof bytes);

    CHECK(gs_store_put_track(s, hash + 1, "another", "cy", bytes, 128));

    gs_track_row rows[8];
    int n = gs_store_list_tracks(s, rows, 8);
    CHECK(n == 2);
    CHECK(rows[0].bytes == 128 || rows[1].bytes == 128);

    gs_store_close(s);
}

TEST(publishing_is_a_separate_thing_from_storing) {
    gs_store *s = gs_store_open(":memory:");
    if (s == nullptr) { gs_failures++; return; }

    uint8_t bytes[256];
    memset(bytes, 0x5a, sizeof bytes);

    const uint64_t mine = 0x1111ull, theirs = 0x2222ull;
    CHECK(gs_store_put_track(s, mine, "the loop", "ada", bytes, sizeof bytes));
    CHECK(gs_store_put_track(s, theirs, "the drop", "bez", bytes, sizeof bytes));

    // **Stored is not published.** The server holds every track it is handed,
    // because it needs them to check times; being listed is a separate choice.
    CHECK(gs_store_track_count(s) == 2);
    CHECK(!gs_store_is_published(s, mine));

    gs_track_row rows[8];
    CHECK(gs_store_list_published(s, rows, 8) == 0);

    CHECK(gs_store_publish(s, mine, "the loop", "ada"));
    CHECK(gs_store_is_published(s, mine));
    CHECK(gs_store_list_published(s, rows, 8) == 1);
    CHECK(rows[0].hash == mine);
    CHECK(strcmp(rows[0].author, "ada") == 0);

    // Publishing something that is not here is not a silent success.
    CHECK(!gs_store_publish(s, 0xdeadull, "nope", "ada"));

    // **Only whoever put it up can take it down.**
    CHECK(!gs_store_withdraw(s, mine, "bez"));
    CHECK(gs_store_is_published(s, mine));
    CHECK(gs_store_withdraw(s, mine, "ada"));
    CHECK(!gs_store_is_published(s, mine));
    CHECK(gs_store_list_published(s, rows, 8) == 0);

    // And the track itself stays, so times set on it are still checkable.
    CHECK(gs_store_has_track(s, mine));
    CHECK(gs_store_track_count(s) == 2);

    // Withdrawing twice is not an error the second time so much as nothing.
    CHECK(!gs_store_withdraw(s, mine, "ada"));

    gs_store_close(s);
}

TEST(a_name_with_a_quote_in_it_is_a_name_and_not_an_instruction) {
    // A driver's name arrives over a network from somebody this server has
    // never met. There is exactly one way to be safe about that and it is bound
    // parameters, so this is what proves they are used: a name that would be
    // catastrophic if it were ever concatenated into a statement.
    gs_store *s = gs_store_open(":memory:");
    if (s == nullptr) { gs_failures++; return; }

    const char *nasty = "'); DROP TABLE driver;--";
    int64_t id = gs_store_put_driver(s, nasty, 1, 1);
    CHECK(id > 0);
    CHECK(gs_store_driver_count(s) == 1);

    char back[GS_STORE_NAME];
    CHECK(gs_store_driver(s, id, back, sizeof back, nullptr, nullptr));
    CHECK(strncmp(back, nasty, sizeof back - 1) == 0);

    // The table is still there, and still works.
    CHECK(gs_store_put_driver(s, "ada", 0, 0) > 0);
    CHECK(gs_store_driver_count(s) == 2);

    // And through a record, which takes a name too.
    CHECK(gs_store_put_record(s, 1, 2, 3, nasty, 0, 100, 200));
    CHECK(gs_store_best_lap(s, 1, 2, nullptr, 0) == 100);
    CHECK(gs_store_record_count(s) == 1);

    gs_store_close(s);
}

TEST(what_is_written_is_still_there_after_a_reopen) {
    // In a file this time, because "it survives" is the one claim :memory:
    // cannot make.
    const char *path = "store_test.db";
    remove(path);

    gs_store *s = gs_store_open(path);
    if (s == nullptr) { gs_failures++; return; }

    CHECK(gs_store_put_driver(s, "ada", 4, 2) > 0);
    CHECK(gs_store_put_record(s, 77, 88, 3, "ada", 2, 4321, 13000));

    uint8_t bytes[64];
    memset(bytes, 0xab, sizeof bytes);
    CHECK(gs_store_put_track(s, 99, "mine", "ada", bytes, sizeof bytes));
    gs_store_close(s);

    gs_store *again = gs_store_open(path);
    CHECK(again != nullptr);
    if (again == nullptr) return;

    CHECK(gs_store_version(again) == 3);
    CHECK(gs_store_driver_count(again) == 1);
    CHECK(gs_store_best_lap(again, 77, 88, nullptr, 0) == 4321);
    CHECK(gs_store_has_track(again, 99));

    gs_store_close(again);
    remove(path);
}

// --- the database that ships -----------------------------------------------

#ifndef GS_SOURCE_ASSETS
#define GS_SOURCE_ASSETS "assets"
#endif

// A working copy. Opening the committed file would leave SQLite's journal
// sidecars beside it and modify the very thing being checked.
static bool gs_copy_shipped(const char *to) {
    FILE *in = fopen(GS_SOURCE_ASSETS "/server/gearstick.db", "rb");
    if (in == nullptr) return false;

    FILE *out = fopen(to, "wb");
    if (out == nullptr) { fclose(in); return false; }

    static uint8_t buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    return ok;
}

TEST(the_shipped_library_is_a_database_this_code_can_still_use) {
    // **The one that stops a committed binary rotting.** The shipped database
    // is built by a tool and checked in, so a schema change here and a forgotten
    // rebuild there would ship a file that fails in somebody's hands rather than
    // in the build. Exercised through the whole API rather than compared as
    // text: what matters is that this code can use that file.
    const char *path = "shipped-copy.db";
    if (!gs_copy_shipped(path)) {
        printf("  FAIL no shipped library at %s\n",
               GS_SOURCE_ASSETS "/server/gearstick.db");
        gs_failures++;
        return;
    }

    gs_store *shipped = gs_store_open(path);
    CHECK(shipped != nullptr);
    if (shipped == nullptr) { remove(path); return; }

    gs_store *fresh = gs_store_open(":memory:");
    CHECK(fresh != nullptr);
    if (fresh != nullptr) {
        CHECK(gs_store_version(shipped) == gs_store_version(fresh));
        gs_store_close(fresh);
    }

    // Every table the server writes to, written to. A column that had moved
    // would fail here rather than the first time somebody set a lap time.
    CHECK(gs_store_put_driver(shipped, "ada", 2, 1) > 0);
    CHECK(gs_store_put_record(shipped, 1234, 1, 3, "ada", 1, 900, 2700));
    CHECK(gs_store_best_lap(shipped, 1234, 1, nullptr, 0) == 900);
    CHECK(gs_store_put_track(shipped, 4321, "scratch", "ada",
                             (const uint8_t *)"not a track", 11));
    CHECK(gs_store_publish(shipped, 4321, "scratch", "ada"));
    CHECK(gs_store_withdraw(shipped, 4321, "ada"));

    gs_store_close(shipped);
    remove(path);
}

TEST(the_shipped_library_holds_the_stock_tracks_and_they_are_tracks) {
    // Not "it has rows in it": every published entry is read back as a track and
    // has to hash to the key it was filed under. A library of blobs that will
    // not load is worse than an empty one, because the empty one is honest.
    const char *path = "shipped-tracks.db";
    if (!gs_copy_shipped(path)) { gs_failures++; return; }

    gs_store *s = gs_store_open(path);
    CHECK(s != nullptr);
    if (s == nullptr) { remove(path); return; }

    static gs_track_row rows[64];
    int n = gs_store_list_published(s, rows, 64);

    // **No clock in it.** The store stamps a track with the time it was added,
    // which is right for a running server and wrong for a file in a repository:
    // it made the shipped library different on every build, so the job that
    // diffs it against what is committed would have failed for ever, and failed
    // about a clock. The tool that builds it dates the stock tracks at the
    // epoch, and a rebuilt library is byte for byte the one that shipped.
    for (int i = 0; i < n; i++) CHECK(gs_store_added(s, rows[i].hash) == 0);

    // A dozen is what a fresh install is promised. There are more than that.
    CHECK(n >= 12);

    static uint8_t bytes[GS_TRACK_TILES * 4 + 4096];
    static gs_track t;
    for (int i = 0; i < n; i++) {
        size_t len = 0;
        CHECK(gs_store_get_track(s, rows[i].hash, bytes, sizeof bytes, &len));
        CHECK(gs_track_deserialize(&t, bytes, len));
        CHECK(gs_track_hash(&t) == rows[i].hash);

        // And it is a track somebody could race: a route, and ground under it.
        CHECK(t.gate_count >= 2);
        CHECK(gs_track_validate(&t).problem == GS_TRACK_OK);
        CHECK(rows[i].name[0] != '\0');
    }

    gs_store_close(s);
    remove(path);
}

// Two people, as keys rather than names - which is the whole change.
static const uint8_t *gs_key(uint8_t who) {
    static uint8_t k[4][GS_STORE_KEY_BYTES];
    for (unsigned i = 0; i < GS_STORE_KEY_BYTES; i++) {
        k[who][i] = (uint8_t)((who * 61u + i * 7u + 3u) & 0xffu);
    }
    return k[who];
}

static bool gs_put_a_track(gs_store *s, uint64_t hash, const char *name) {
    uint8_t bytes[64];
    for (size_t i = 0; i < sizeof bytes; i++) bytes[i] = (uint8_t)(hash + i);
    return gs_store_put_track(s, hash, name, "", bytes, sizeof bytes);
}

TEST(a_profile_with_a_password_cannot_be_used_without_it) {
    gs_store *s = gs_store_open(":memory:");
    CHECK(s != nullptr);
    if (s == nullptr) return;

    // **A driver with no password still works.** This is first because it is
    // the case that must never break: a racing game that demands an account
    // before anybody can drive has lost the argument.
    CHECK(!gs_store_has_password(s, "cy"));

    char hash[GS_AUTH_HASH_BYTES];
    CHECK(gs_auth_hash_password("the quick brown fox", hash, sizeof hash));
    CHECK(gs_store_set_password(s, "ada", hash));
    CHECK(gs_store_has_password(s, "ada"));

    // The password itself is nowhere in what was stored.
    char kept[GS_AUTH_HASH_BYTES];
    CHECK(gs_store_password(s, "ada", kept, sizeof kept));
    CHECK(strstr(kept, "the quick brown fox") == nullptr);

    // **Two hashes of the same password differ**, because the salt is inside
    // them - so a stolen database does not say which two people chose the same
    // one.
    char again[GS_AUTH_HASH_BYTES];
    CHECK(gs_auth_hash_password("the quick brown fox", again, sizeof again));
    CHECK(strcmp(again, kept) != 0);

    CHECK(gs_auth_check_password(kept, "the quick brown fox"));
    CHECK(!gs_auth_check_password(kept, "the quick brown fo"));
    CHECK(!gs_auth_check_password(kept, "the quick brown fox "));
    CHECK(!gs_auth_check_password(kept, ""));

    // Taken off again, and the name is ordinary once more.
    CHECK(gs_store_set_password(s, "ada", nullptr));
    CHECK(!gs_store_has_password(s, "ada"));

    gs_store_close(s);
}

TEST(the_one_time_code_is_the_one_rfc_6238_publishes) {
    // **RFC 6238 appendix B**, the SHA-256 column, at this project's six digits
    // rather than the specification's eight - which is why these are the last
    // six of the published values. Checked against Python's `hmac` as well,
    // which is a different implementation by different people, and it agrees
    // with both.
    //
    // The seed is the specification's: thirty-two ASCII digits.
    const uint8_t *seed = (const uint8_t *)"12345678901234567890123456789012";
    const size_t len = 32;

    static const struct { int64_t at; uint32_t code; } v[] = {
        {         59, 119246 },      // published 46119246
        { 1111111109,  84774 },      // published 68084774
        { 1234567890, 819424 },      // published 91819424
        { 2000000000, 698825 },      // published 90698825
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        int64_t step = gs_auth_step_of(v[i].at);
        CHECK(gs_auth_code_at(seed, len, step) == v[i].code);
    }

    // And the time step is the specification's thirty seconds.
    CHECK(gs_auth_step_of(59) == 1);
    CHECK(gs_auth_step_of(60) == 2);
}

TEST(a_one_time_code_works_once_inside_the_window_it_is_valid_for) {
    gs_store *s = gs_store_open(":memory:");
    CHECK(s != nullptr);
    if (s == nullptr) return;

    uint8_t secret[GS_AUTH_SECRET_BYTES];
    gs_auth_new_secret(secret, sizeof secret);
    CHECK(gs_store_set_totp(s, "ada", secret, sizeof secret));

    uint8_t back[GS_AUTH_SECRET_BYTES];
    size_t len = 0;
    CHECK(gs_store_totp(s, "ada", back, sizeof back, &len));
    CHECK(len == sizeof secret);
    CHECK(memcmp(back, secret, len) == 0);

    // A fixed moment, so this does not depend on when it runs.
    const int64_t now = 1700000000;
    int64_t step = gs_auth_step_of(now);

    uint32_t code = gs_auth_code_at(secret, sizeof secret, step);
    CHECK(code < 1000000u);

    int64_t used = 0;
    CHECK(gs_auth_check_code(secret, sizeof secret, code, now, 1, &used));
    CHECK(used == step);

    // **Spent once.** The window is still open - the same thirty seconds, the
    // same code - and it does not work a second time, which is the whole point
    // of remembering which step was used.
    CHECK(gs_store_totp_use(s, "ada", used));
    CHECK(!gs_store_totp_use(s, "ada", used));
    CHECK(gs_auth_check_code(secret, sizeof secret, code, now + 5, 1, &used));
    CHECK(!gs_store_totp_use(s, "ada", used));

    // The next step is a different code, and it can be spent.
    uint32_t next = gs_auth_code_at(secret, sizeof secret, step + 1);
    CHECK(next != code);
    CHECK(gs_store_totp_use(s, "ada", step + 1));

    // And a step already behind is refused, so a captured code from a minute
    // ago is worth nothing even if the clock slack would have reached it.
    CHECK(!gs_store_totp_use(s, "ada", step));

    // A wrong code is a wrong code.
    CHECK(!gs_auth_check_code(secret, sizeof secret, code ^ 1u, now, 1, &used));

    // A little clock skew is allowed and a lot is not - a second factor that
    // refuses a phone eleven seconds fast is one nobody can use.
    uint32_t earlier = gs_auth_code_at(secret, sizeof secret, step - 1);
    CHECK(gs_auth_check_code(secret, sizeof secret, earlier, now, 1, &used));
    CHECK(!gs_auth_check_code(secret, sizeof secret, earlier, now, 0, &used));

    gs_store_close(s);
}

TEST(a_track_belongs_to_whoever_built_it_and_to_nobody_else) {
    gs_store *s = gs_store_open(":memory:");
    CHECK(s != nullptr);
    if (s == nullptr) return;

    const uint8_t *ada = gs_key(0), *bez = gs_key(1);

    CHECK(gs_put_a_track(s, 0x1001, "ada's oval"));
    CHECK(gs_store_claim_track(s, 0x1001, ada));

    // **The first claim wins.** A track is content-addressed, so two people who
    // built the same thing built the same thing - and the second to upload it
    // does not take it off the first.
    CHECK(!gs_store_claim_track(s, 0x1001, bez));
    CHECK(gs_store_claim_track(s, 0x1001, ada));     // and it is idempotent

    uint8_t owner[GS_STORE_KEY_BYTES];
    CHECK(gs_store_track_owner(s, 0x1001, owner));
    CHECK(memcmp(owner, ada, GS_STORE_KEY_BYTES) == 0);

    // Nobody else can change it, take it down, or delete it.
    CHECK(!gs_store_set_visible(s, 0x1001, bez, GS_TRACK_PUBLIC));
    CHECK(!gs_store_share_track(s, 0x1001, bez, bez));
    CHECK(!gs_store_delete_track(s, 0x1001, bez));

    // And the owner can do all three.
    CHECK(gs_store_set_visible(s, 0x1001, ada, GS_TRACK_PUBLIC));
    CHECK(gs_store_is_published(s, 0x1001));
    CHECK(gs_store_set_visible(s, 0x1001, ada, GS_TRACK_PRIVATE));
    CHECK(!gs_store_is_published(s, 0x1001));
    CHECK(gs_store_delete_track(s, 0x1001, ada));
    CHECK(!gs_store_has_track(s, 0x1001));

    gs_store_close(s);
}

TEST(a_shared_track_is_visible_to_exactly_who_it_was_shared_with) {
    gs_store *s = gs_store_open(":memory:");
    CHECK(s != nullptr);
    if (s == nullptr) return;

    const uint8_t *ada = gs_key(0), *bez = gs_key(1), *cy = gs_key(2);

    CHECK(gs_put_a_track(s, 0x2001, "the long way round"));
    CHECK(gs_store_claim_track(s, 0x2001, ada));

    // Private: the owner and nobody else, including somebody with no identity
    // at all.
    CHECK(gs_store_can_see(s, 0x2001, ada));
    CHECK(!gs_store_can_see(s, 0x2001, bez));
    CHECK(!gs_store_can_see(s, 0x2001, cy));
    CHECK(!gs_store_can_see(s, 0x2001, nullptr));

    // Shared with bez, and *only* bez. Sharing without saying so is the failure
    // this is guarding: a "shared" flag with no list means shared with anybody
    // who asks.
    CHECK(gs_store_set_visible(s, 0x2001, ada, GS_TRACK_SHARED));
    CHECK(gs_store_share_track(s, 0x2001, ada, bez));
    CHECK(gs_store_can_see(s, 0x2001, ada));
    CHECK(gs_store_can_see(s, 0x2001, bez));
    CHECK(!gs_store_can_see(s, 0x2001, cy));
    CHECK(!gs_store_can_see(s, 0x2001, nullptr));

    // Taken back.
    CHECK(gs_store_unshare_track(s, 0x2001, ada, bez));
    CHECK(!gs_store_can_see(s, 0x2001, bez));

    // Public: everybody, and nobody needs to be anybody.
    CHECK(gs_store_set_visible(s, 0x2001, ada, GS_TRACK_PUBLIC));
    CHECK(gs_store_can_see(s, 0x2001, cy));
    CHECK(gs_store_can_see(s, 0x2001, nullptr));

    // And the listing agrees with the question, which is the part that would
    // otherwise drift: two different pieces of SQL answering "who may see this"
    // differently is how a private track ends up in somebody's list.
    CHECK(gs_store_set_visible(s, 0x2001, ada, GS_TRACK_SHARED));
    CHECK(gs_store_share_track(s, 0x2001, ada, bez));

    gs_track_row rows[8];
    CHECK(gs_store_list_visible(s, bez, rows, 8) == 1);
    CHECK(gs_store_list_visible(s, cy, rows, 8) == 0);
    CHECK(gs_store_list_visible(s, ada, rows, 8) == 1);
    CHECK(gs_store_list_visible(s, nullptr, rows, 8) == 0);

    gs_store_close(s);
}

// A database in the shape this code used to make, before tracks had an owner,
// a shipped flag or a visibility - written with SQLite directly, because the
// point is to produce something *this* code did not.
static bool gs_old_store(const char *path) {
    remove(path);

    sqlite3 *db = nullptr;
    if (sqlite3_open(path, &db) != SQLITE_OK) return false;

    // The schema as it was: a track had `published` and nothing else about who
    // could see it. Everything a store carried then, so that what survives the
    // migration can be checked rather than assumed.
    const char *sql =
        "CREATE TABLE meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);"
        "INSERT INTO meta (key, value) VALUES ('schema', 1);"
        "CREATE TABLE driver ("
        "  name  TEXT PRIMARY KEY,"
        "  seen  INTEGER NOT NULL DEFAULT 0,"
        "  races INTEGER NOT NULL DEFAULT 0,"
        "  wins  INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO driver (name, races, wins) VALUES ('ada', 4, 2);"
        "CREATE TABLE track ("
        "  hash      INTEGER PRIMARY KEY,"
        "  name      TEXT NOT NULL DEFAULT '',"
        "  author    TEXT NOT NULL DEFAULT '',"
        "  added     INTEGER NOT NULL DEFAULT 0,"
        "  published INTEGER NOT NULL DEFAULT 0,"
        "  bytes     BLOB NOT NULL);"
        "INSERT INTO track (hash, name, author, published, bytes)"
        "  VALUES (111, 'up', 'ada', 1, x'0102030405');"
        "INSERT INTO track (hash, name, author, published, bytes)"
        "  VALUES (222, 'mine', 'ada', 0, x'0607080910');";

    char *err = nullptr;
    const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, &err) == SQLITE_OK;
    sqlite3_free(err);
    sqlite3_close(db);
    return ok;
}

TEST(a_store_from_an_older_build_keeps_everything_it_had) {
    // **Nobody had ever opened an old one.** Every test here makes a fresh
    // database, so the migration - six columns added, and the old `published`
    // flag turned into the new visibility - had only ever run against a
    // database that already had all of them, where it does nothing at all.
    //
    // What it is for is the person upgrading. Their store is the old shape,
    // and if this is wrong their tracks are gone or their published ones have
    // quietly gone private, on a server they were running for other people.
    const char *path = "store_old.db";
    if (!gs_old_store(path)) { gs_failures++; return; }

    gs_store *s = gs_store_open(path);
    CHECK(s != nullptr);
    if (s == nullptr) return;

    // It came forward to today's schema rather than being refused.
    CHECK(gs_store_version(s) == 3);

    // **Everything that was in it is still in it.**
    CHECK(gs_store_driver_count(s) == 1);
    CHECK(gs_store_track_count(s) == 2);

    uint8_t bytes[64];
    size_t n = 0;
    CHECK(gs_store_get_track(s, 111, bytes, sizeof bytes, &n));
    CHECK(n == 5);
    CHECK(bytes[0] == 0x01 && bytes[4] == 0x05);
    CHECK(gs_store_get_track(s, 222, bytes, sizeof bytes, &n));
    CHECK(n == 5);
    CHECK(bytes[0] == 0x06);

    // **And what was up is still up, and what was not is not.** This is the
    // half that would go wrong silently: a track somebody published before the
    // upgrade going private is not a crash, it is a track that stops being
    // there for everybody else.
    gs_track_row rows[8];
    const int up = gs_store_list_published(s, rows, 8);
    CHECK(up == 1);
    if (up == 1) {
        CHECK(rows[0].hash == 111);
        CHECK(strcmp(rows[0].name, "up") == 0);
    }
    CHECK(gs_store_is_published(s, 111));
    CHECK(!gs_store_is_published(s, 222));

    // Nobody owns them, which is what an old row means: they were stored
    // before a track had an owner at all.
    uint8_t owner[GS_STORE_KEY_BYTES];
    CHECK(!gs_store_track_owner(s, 111, owner));
    CHECK(!gs_store_track_owner(s, 222, owner));

    // And the upgrade is not a one-off: closing and opening it again is the
    // same store, not a second migration doing something else.
    gs_store_close(s);
    s = gs_store_open(path);
    CHECK(s != nullptr);
    if (s == nullptr) return;
    CHECK(gs_store_track_count(s) == 2);
    CHECK(gs_store_list_published(s, rows, 8) == 1);
    gs_store_close(s);
    remove(path);
}

TEST(a_track_that_shipped_with_the_game_is_outside_all_of_it) {
    gs_store *s = gs_store_open(":memory:");
    CHECK(s != nullptr);
    if (s == nullptr) return;

    const uint8_t *ada = gs_key(0);

    // Exactly as the library builder does it: stored, published, then put
    // outside ownership. Published matters - a track that was never up cannot
    // be withdrawn for that reason alone, and the check below would then pass
    // without the rule it is testing existing at all.
    CHECK(gs_put_a_track(s, 0x3001, "head on"));
    CHECK(gs_store_publish(s, 0x3001, "head on", "gearstick"));
    CHECK(gs_store_mark_shipped(s, 0x3001));
    CHECK(gs_store_track_is_shipped(s, 0x3001));
    CHECK(gs_store_is_published(s, 0x3001));

    // **Nobody owns it**, and that is not an owner nobody got round to setting.
    CHECK(!gs_store_track_owner(s, 0x3001, nullptr));

    // Every write path refuses it, whoever is asking - including the profile
    // that happens to be called the same thing as its author, because the check
    // is not about a name at all.
    CHECK(!gs_store_claim_track(s, 0x3001, ada));
    CHECK(!gs_store_set_visible(s, 0x3001, ada, GS_TRACK_PRIVATE));
    CHECK(!gs_store_share_track(s, 0x3001, ada, ada));
    CHECK(!gs_store_unshare_track(s, 0x3001, ada, ada));
    CHECK(!gs_store_delete_track(s, 0x3001, ada));
    // Literally the author string the stock library carries. This is the case
    // the plan names: somebody whose profile happens to be called the same
    // thing as a stock track's author still cannot touch it, because the check
    // was never about the name.
    CHECK(!gs_store_withdraw(s, 0x3001, "gearstick"));

    // It is still there, and everybody can still see it.
    CHECK(gs_store_has_track(s, 0x3001));
    CHECK(gs_store_can_see(s, 0x3001, ada));
    CHECK(gs_store_can_see(s, 0x3001, nullptr));

    gs_store_close(s);
}

TEST(a_session_token_is_good_once_and_only_for_who_it_was_issued_to) {
    gs_store *s = gs_store_open(":memory:");
    CHECK(s != nullptr);
    if (s == nullptr) return;

    const int64_t now = 1700000000;
    const int64_t hour = 3600;

    CHECK(gs_store_issue_session(s, 0x1111, "ada", now, hour));
    CHECK(gs_store_session_count(s) == 1);

    // The same nonce twice is a collision, and the caller is told so rather
    // than quietly overwriting somebody else's session.
    CHECK(!gs_store_issue_session(s, 0x1111, "bez", now, hour));

    // **Issued to somebody else buys nothing**, which is the whole point: a
    // token seen on the wire is no use to whoever saw it.
    CHECK(!gs_store_spend_session(s, 0x1111, "bez", now));

    // Nor one that was never issued.
    CHECK(!gs_store_spend_session(s, 0x2222, "ada", now));

    // Nor one that has expired.
    CHECK(!gs_store_spend_session(s, 0x1111, "ada", now + hour + 1));

    // Ada's own, in date: once.
    CHECK(gs_store_spend_session(s, 0x1111, "ada", now + 10));
    CHECK(!gs_store_spend_session(s, 0x1111, "ada", now + 11));

    // Expired ones are swept, and a live one is left alone.
    CHECK(gs_store_issue_session(s, 0x3333, "ada", now, 10));
    CHECK(gs_store_issue_session(s, 0x4444, "ada", now, hour));
    CHECK(gs_store_forget_sessions(s, now + 20) >= 1);
    CHECK(gs_store_spend_session(s, 0x4444, "ada", now + 20));

    gs_store_close(s);
}

TEST(a_session_outlives_the_process_that_issued_it) {
    // **The reason sessions are in the database.** A server that kept them in
    // memory would forget every one on restart, and a nonce nobody can retire is
    // a nonce that can be handed in for ever - which is exactly what it exists
    // to stop.
    const char *path = "sessions.db";
    remove(path);
    remove("sessions.db-wal");
    remove("sessions.db-shm");

    const int64_t now = 1700000000;

    gs_store *first = gs_store_open(path);
    CHECK(first != nullptr);
    if (first == nullptr) return;
    CHECK(gs_store_issue_session(first, 0xabcd, "ada", now, 3600));
    CHECK(gs_store_spend_session(first, 0xabcd, "ada", now + 1));
    gs_store_close(first);

    // A new process, the same store: the token is still spent.
    gs_store *again = gs_store_open(path);
    CHECK(again != nullptr);
    if (again != nullptr) {
        CHECK(!gs_store_spend_session(again, 0xabcd, "ada", now + 2));
        CHECK(gs_store_session_count(again) == 1);
        gs_store_close(again);
    }

    remove(path);
    remove("sessions.db-wal");
    remove("sessions.db-shm");
}

int main(void) {
    printf("gearstick store tests\n");

    run_a_store_opens_and_knows_what_it_is();
    run_a_driver_is_remembered_once_however_often_they_appear();
    run_a_record_is_a_time_on_a_track_under_conditions_over_a_distance();
    run_a_track_is_stored_by_what_it_is();
    run_publishing_is_a_separate_thing_from_storing();
    run_a_name_with_a_quote_in_it_is_a_name_and_not_an_instruction();
    run_what_is_written_is_still_there_after_a_reopen();
    run_a_profile_with_a_password_cannot_be_used_without_it();
    run_the_one_time_code_is_the_one_rfc_6238_publishes();
    run_a_one_time_code_works_once_inside_the_window_it_is_valid_for();
    run_a_track_belongs_to_whoever_built_it_and_to_nobody_else();
    run_a_shared_track_is_visible_to_exactly_who_it_was_shared_with();
    run_a_store_from_an_older_build_keeps_everything_it_had();
    run_a_track_that_shipped_with_the_game_is_outside_all_of_it();
    run_a_session_token_is_good_once_and_only_for_who_it_was_issued_to();
    run_a_session_outlives_the_process_that_issued_it();
    run_the_shipped_library_is_a_database_this_code_can_still_use();
    run_the_shipped_library_holds_the_stock_tracks_and_they_are_tracks();

    if (gs_failures > 0) {
        printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
        return 1;
    }
    printf("all store tests passed\n");
    return 0;
}

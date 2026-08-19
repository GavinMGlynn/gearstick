// test_store.c - what the server remembers, and the rules it remembers it by.
//
// Every test runs against an in-memory database, so nothing here depends on a
// file left behind by the last run - which is the failure mode that makes a
// storage test pass on one machine and fail on a fresh checkout.
#include "net/gs_store.h"

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

    CHECK(gs_store_version(s) == 1);
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

    CHECK(gs_store_version(again) == 1);
    CHECK(gs_store_driver_count(again) == 1);
    CHECK(gs_store_best_lap(again, 77, 88, nullptr, 0) == 4321);
    CHECK(gs_store_has_track(again, 99));

    gs_store_close(again);
    remove(path);
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

    if (gs_failures > 0) {
        printf("%d check%s failed\n", gs_failures, gs_failures == 1 ? "" : "s");
        return 1;
    }
    printf("all store tests passed\n");
    return 0;
}

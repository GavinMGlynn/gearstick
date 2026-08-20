// make_store - the database a server ships with.
//
// **A server that has never been run still has a library to offer.** Everything
// the server knows lives in its database; a fresh one that knew nothing would
// mean the first person to connect finds an empty list and no reason to stay.
// So the stock tracks go in here, published, and the built file is committed.
//
// Committed as a binary, which is a thing to be careful with: a database that
// drifts from the schema `gs_store.c` writes would fail in the field rather than
// in the build. Two things stop that. It is built by this tool through the same
// gs_store API the server uses, so there is no second copy of the schema to
// disagree with; and a test compares the shipped file against a freshly created
// one, so a schema change that forgets this file turns the tree red.
//
// Regenerate with:
//
//     cmake -B build -DGEARSTICK_SQLITE_AMALGAMATION=ON
//     cmake --build build --target gearstick_make_tracks gearstick_make_store
//     ./build/gearstick_make_tracks assets/tracks
//     ./build/gearstick_make_store assets/server/gearstick.db assets/tracks/*.gstrack
//
// **The amalgamation, and not whatever SQLite the machine has.** The file is
// written from nothing every time, so its bytes depend on the tracks and on the
// version of SQLite that laid the pages out - and those are not the same
// version everywhere. A system 3.45 and a pinned 3.53 store identical rows in a
// different arrangement of bytes, which is invisible to every query and fatal to
// a job that diffs the file against what is committed. Pinning the writer is
// what makes the artefact reproducible, exactly as the committed trig table is
// pinned to the script that bakes it.
#include "core/gs_track.h"
#include "net/gs_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t gs_bytes[GS_TRACK_TILES * 4 + 4096];
static gs_track gs_t;

// Who the stock tracks belong to. Publishing is "somebody put this up", and a
// withdraw is only allowed by whoever did - so an author nobody can log in as
// is what keeps the shipped set from being taken down by a passing client.
#define GS_STOCK_AUTHOR "gearstick"

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: %s <out.db> <track.gstrack>...\n", argv[0]);
        return 2;
    }

    // From nothing: an existing file would leave whatever was in it behind, and
    // a shipped database is defined by what this run put in it.
    remove(argv[1]);

    gs_store *s = gs_store_open(argv[1]);
    if (s == nullptr) {
        printf("could not create %s\n", argv[1]);
        return 1;
    }

    int written = 0;
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (f == nullptr) {
            printf("could not read %s\n", argv[i]);
            gs_store_close(s);
            return 1;
        }
        size_t len = fread(gs_bytes, 1, sizeof gs_bytes, f);
        fclose(f);

        // Read it rather than trusting it: the name and the hash both come from
        // the track itself, so a file that is not one stops this here instead of
        // shipping as an entry nobody can load.
        if (!gs_track_deserialize(&gs_t, gs_bytes, len)) {
            printf("%s is not a track\n", argv[i]);
            gs_store_close(s);
            return 1;
        }
        uint64_t hash = gs_track_hash(&gs_t);

        // The file's own name, without directory or extension. A track file is
        // named for the track, and the alternative is a second place to keep the
        // name where the two can disagree.
        const char *base = argv[i];
        for (const char *c = argv[i]; *c != '\0'; c++) {
            if (*c == '/' || *c == '\\') base = c + 1;
        }
        char name[64];
        size_t n = 0;
        for (const char *c = base; *c != '\0' && *c != '.' && n + 1 < sizeof name; c++) {
            name[n++] = *c == '-' ? ' ' : *c;
        }
        name[n] = '\0';

        if (!gs_store_put_track(s, hash, name, GS_STOCK_AUTHOR, gs_bytes, len) ||
            !gs_store_publish(s, hash, name, GS_STOCK_AUTHOR) ||
            // **And outside ownership for good.** A stock track with an owner
            // is a stock track somebody can take down; one merely published is
            // one that a profile called "gearstick" could withdraw.
            !gs_store_mark_shipped(s, hash) ||
            // **Dated at the epoch, because a committed file has to be the same
            // file every time it is built.** The store stamps `added` with the
            // time of day, which is the one thing in the schema that changes on
            // its own - so the shipped library came out different every run, the
            // job that diffs it against the repository would have failed for
            // ever, and it would have been failing about a clock. The date a
            // stock track was added means nothing: it shipped with the game.
            !gs_store_set_added(s, hash, 0)) {
            printf("could not store %s: %s\n", argv[i], gs_store_error(s));
            gs_store_close(s);
            return 1;
        }

        printf("  %-24s %2u x %-3u %5zu bytes  %016llx\n", name, gs_t.w, gs_t.h,
               len, (unsigned long long)hash);
        written++;
    }

    printf("%s: %d track(s), all published\n", argv[1], written);
    gs_store_close(s);
    return 0;
}

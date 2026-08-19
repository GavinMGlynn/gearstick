#include "net/gs_store.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GS_SCHEMA 1

struct gs_store {
    sqlite3 *db;
    char     error[256];
};

static void gs_fail(gs_store *s, const char *what) {
    snprintf(s->error, sizeof s->error, "%s: %s", what,
             s->db != nullptr ? sqlite3_errmsg(s->db) : "no database");
}

const char *gs_store_error(const gs_store *s) {
    return (s != nullptr && s->error[0] != '\0') ? s->error : nullptr;
}

// **Every statement is prepared with bound parameters, never assembled.** A
// driver's name arrives over a network from somebody this server has never met,
// and a name is a string. There is exactly one way to be safe about that and it
// is not escaping.
static bool gs_exec(gs_store *s, const char *sql) {
    char *why = nullptr;
    if (sqlite3_exec(s->db, sql, nullptr, nullptr, &why) != SQLITE_OK) {
        snprintf(s->error, sizeof s->error, "%s", why != nullptr ? why : "?");
        sqlite3_free(why);
        return false;
    }
    return true;
}

static bool gs_migrate(gs_store *s) {
    // The schema, and the version it is. Written once here rather than grown by
    // hand later: an upgrade path is a thing to add when there is a version to
    // upgrade *from*, and pretending otherwise produces migration code nobody
    // has ever run.
    if (!gs_exec(s,
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=NORMAL;"
        "PRAGMA foreign_keys=ON;"

        "CREATE TABLE IF NOT EXISTS meta ("
        "  key TEXT PRIMARY KEY, value INTEGER NOT NULL);"

        "CREATE TABLE IF NOT EXISTS driver ("
        "  id      INTEGER PRIMARY KEY,"
        "  name    TEXT NOT NULL UNIQUE,"
        "  colour  INTEGER NOT NULL DEFAULT 0,"
        "  vehicle INTEGER NOT NULL DEFAULT 0,"
        "  seen    INTEGER NOT NULL DEFAULT 0);"

        // A record is a time on a track under conditions over a distance, by
        // somebody. All five together are the key - that is not normalisation
        // pedantry, it is the difference between a leaderboard and a mess.
        "CREATE TABLE IF NOT EXISTS record ("
        "  track      INTEGER NOT NULL,"
        "  conditions INTEGER NOT NULL,"
        "  laps       INTEGER NOT NULL,"
        "  driver     INTEGER NOT NULL REFERENCES driver(id),"
        "  vehicle    INTEGER NOT NULL,"
        "  lap_ticks  INTEGER NOT NULL DEFAULT 0,"
        "  race_ticks INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY (track, conditions, laps, driver));"

        "CREATE INDEX IF NOT EXISTS record_by_lap"
        "  ON record (track, conditions, lap_ticks);"

        // Content-addressed, so uploading the same track twice stores it once.
        "CREATE TABLE IF NOT EXISTS track ("
        "  hash      INTEGER PRIMARY KEY,"
        "  name      TEXT NOT NULL DEFAULT '',"
        "  author    TEXT NOT NULL DEFAULT '',"
        "  added     INTEGER NOT NULL DEFAULT 0,"
        // Published is a separate thing from stored. The server holds every
        // track it has been handed - it needs them to verify times - and shows
        // only the ones somebody chose to put up.
        "  published INTEGER NOT NULL DEFAULT 0,"
        "  bytes     BLOB NOT NULL);"

        // **A session is a nonce the server issued to somebody, once.**
        //
        // In the database rather than in memory, for the reason everything else
        // here is: a server that forgot its sessions on restart could not say
        // whether a nonce had already been spent, and a nonce nobody can retire
        // is a nonce that can be handed in for ever - which is the whole thing
        // it exists to stop.
        "CREATE TABLE IF NOT EXISTS session ("
        "  nonce   INTEGER PRIMARY KEY,"
        "  driver  TEXT NOT NULL,"
        "  issued  INTEGER NOT NULL,"
        "  expires INTEGER NOT NULL,"
        "  spent   INTEGER NOT NULL DEFAULT 0);"

        "CREATE INDEX IF NOT EXISTS session_by_driver ON session (driver);")) {
        return false;
    }

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO meta (key, value) VALUES ('schema', ?1)"
            "  ON CONFLICT(key) DO NOTHING",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "schema");
        return false;
    }
    sqlite3_bind_int(st, 1, GS_SCHEMA);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

gs_store *gs_store_open(const char *path) {
    gs_store *s = (gs_store *)calloc(1, sizeof *s);
    if (s == nullptr) return nullptr;

    if (sqlite3_open(path, &s->db) != SQLITE_OK) {
        gs_fail(s, "open");
        sqlite3_close(s->db);
        free(s);
        return nullptr;
    }
    if (!gs_migrate(s)) {
        sqlite3_close(s->db);
        free(s);
        return nullptr;
    }
    return s;
}

void gs_store_close(gs_store *s) {
    if (s == nullptr) return;
    sqlite3_close(s->db);
    free(s);
}

int gs_store_version(const gs_store *s) {
    if (s == nullptr) return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT value FROM meta WHERE key='schema'",
                           -1, &st, nullptr) != SQLITE_OK) {
        return 0;
    }
    int v = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return v;
}

static int gs_count(gs_store *s, const char *sql) {
    if (s == nullptr) return 0;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, nullptr) != SQLITE_OK) return 0;
    int n = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return n;
}

// --- drivers ---------------------------------------------------------------

int64_t gs_store_find_driver(gs_store *s, const char *name) {
    if (s == nullptr || name == nullptr) return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT id FROM driver WHERE name = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "find driver");
        return 0;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);

    int64_t id = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    return id;
}

int64_t gs_store_put_driver(gs_store *s, const char *name, uint8_t colour,
                            uint8_t vehicle) {
    if (s == nullptr || name == nullptr || name[0] == '\0') return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO driver (name, colour, vehicle, seen)"
            "  VALUES (?1, ?2, ?3, 1)"
            "  ON CONFLICT(name) DO UPDATE SET"
            "    colour = ?2, vehicle = ?3, seen = seen + 1",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "put driver");
        return 0;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, colour);
    sqlite3_bind_int(st, 3, vehicle);

    if (sqlite3_step(st) != SQLITE_DONE) {
        gs_fail(s, "put driver");
        sqlite3_finalize(st);
        return 0;
    }
    sqlite3_finalize(st);
    return gs_store_find_driver(s, name);
}

bool gs_store_driver(gs_store *s, int64_t id, char *name, size_t cap,
                     uint8_t *colour, uint8_t *vehicle) {
    if (s == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "SELECT name, colour, vehicle FROM driver WHERE id = ?1",
            -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, id);

    bool got = sqlite3_step(st) == SQLITE_ROW;
    if (got) {
        const unsigned char *n = sqlite3_column_text(st, 0);
        if (name != nullptr && cap > 0) {
            snprintf(name, cap, "%s", n != nullptr ? (const char *)n : "");
        }
        if (colour != nullptr) *colour = (uint8_t)sqlite3_column_int(st, 1);
        if (vehicle != nullptr) *vehicle = (uint8_t)sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);
    return got;
}

int gs_store_driver_count(gs_store *s) {
    return gs_count(s, "SELECT COUNT(*) FROM driver");
}

// --- records ---------------------------------------------------------------

bool gs_store_put_record(gs_store *s, uint64_t track, uint64_t conditions,
                         uint16_t laps, const char *who, uint8_t vehicle,
                         uint32_t lap_ticks, uint32_t race_ticks) {
    if (s == nullptr) return false;

    int64_t driver = gs_store_put_driver(s, who, 0, vehicle);
    if (driver == 0) return false;

    // Kept only where it is better. Doing that in the statement rather than by
    // reading first and writing after means two results arriving together
    // cannot lose one of them.
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO record (track, conditions, laps, driver, vehicle,"
            "                    lap_ticks, race_ticks)"
            "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
            "  ON CONFLICT(track, conditions, laps, driver) DO UPDATE SET"
            "    vehicle = ?5,"
            "    lap_ticks = CASE"
            "      WHEN ?6 > 0 AND (lap_ticks = 0 OR ?6 < lap_ticks) THEN ?6"
            "      ELSE lap_ticks END,"
            "    race_ticks = CASE"
            "      WHEN ?7 > 0 AND (race_ticks = 0 OR ?7 < race_ticks) THEN ?7"
            "      ELSE race_ticks END",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "put record");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)track);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)conditions);
    sqlite3_bind_int(st, 3, laps);
    sqlite3_bind_int64(st, 4, driver);
    sqlite3_bind_int(st, 5, vehicle);
    sqlite3_bind_int64(st, 6, lap_ticks);
    sqlite3_bind_int64(st, 7, race_ticks);

    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) gs_fail(s, "put record");
    sqlite3_finalize(st);
    return ok;
}

static uint32_t gs_best(gs_store *s, const char *sql, uint64_t track,
                        uint64_t conditions, int laps, char *who, size_t cap) {
    if (s == nullptr) return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, sql, -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "best");
        return 0;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)track);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)conditions);
    if (laps >= 0) sqlite3_bind_int(st, 3, laps);

    uint32_t best = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        best = (uint32_t)sqlite3_column_int64(st, 0);
        const unsigned char *n = sqlite3_column_text(st, 1);
        if (who != nullptr && cap > 0) {
            snprintf(who, cap, "%s", n != nullptr ? (const char *)n : "");
        }
    }
    sqlite3_finalize(st);
    return best;
}

uint32_t gs_store_best_lap(gs_store *s, uint64_t track, uint64_t conditions,
                           char *who, size_t cap) {
    // Across every distance: a lap is a lap however long the race was.
    return gs_best(s,
        "SELECT r.lap_ticks, d.name FROM record r JOIN driver d ON d.id = r.driver"
        " WHERE r.track = ?1 AND r.conditions = ?2 AND r.lap_ticks > 0"
        " ORDER BY r.lap_ticks ASC LIMIT 1",
        track, conditions, -1, who, cap);
}

uint32_t gs_store_best_race(gs_store *s, uint64_t track, uint64_t conditions,
                            uint16_t laps, char *who, size_t cap) {
    // A race time only means anything against a race of the same length.
    return gs_best(s,
        "SELECT r.race_ticks, d.name FROM record r JOIN driver d ON d.id = r.driver"
        " WHERE r.track = ?1 AND r.conditions = ?2 AND r.laps = ?3"
        "   AND r.race_ticks > 0"
        " ORDER BY r.race_ticks ASC LIMIT 1",
        track, conditions, (int)laps, who, cap);
}

int gs_store_record_count(gs_store *s) {
    return gs_count(s, "SELECT COUNT(*) FROM record");
}

// --- tracks ----------------------------------------------------------------

bool gs_store_put_track(gs_store *s, uint64_t hash, const char *name,
                        const char *author, const uint8_t *bytes, size_t len) {
    if (s == nullptr || bytes == nullptr || len == 0) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO track (hash, name, author, added, bytes)"
            "  VALUES (?1, ?2, ?3, strftime('%s','now'), ?4)"
            "  ON CONFLICT(hash) DO NOTHING",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "put track");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_text(st, 2, name != nullptr ? name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, author != nullptr ? author : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(st, 4, bytes, (int)len, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) gs_fail(s, "put track");
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_get_track(gs_store *s, uint64_t hash, uint8_t *out, size_t cap,
                        size_t *len) {
    if (s == nullptr) return false;
    if (len != nullptr) *len = 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT bytes FROM track WHERE hash = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);

    bool got = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        size_t n = (size_t)sqlite3_column_bytes(st, 0);
        if (len != nullptr) *len = n;

        got = true;
        if (out != nullptr) {
            if (n > cap) got = false;
            else memcpy(out, blob, n);
        }
    }
    sqlite3_finalize(st);
    return got;
}

// --- sessions ---------------------------------------------------------------

bool gs_store_issue_session(gs_store *s, uint64_t nonce, const char *who,
                            int64_t now, int64_t lifetime) {
    if (s == nullptr || who == nullptr || nonce == 0) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO session (nonce, driver, issued, expires, spent)"
            "  VALUES (?1, ?2, ?3, ?4, 0)"
            "  ON CONFLICT(nonce) DO NOTHING",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "issue session");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)nonce);
    sqlite3_bind_text(st, 2, who, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, now + lifetime);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) > 0;
    if (!ok) gs_fail(s, "issue session");
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_spend_session(gs_store *s, uint64_t nonce, const char *who,
                            int64_t now) {
    if (s == nullptr || who == nullptr || nonce == 0) return false;

    // **All four conditions in the statement, and the change count is the
    // answer.** Reading the row and then updating it would be two steps with a
    // gap between them, and the gap is where the same nonce gets spent twice.
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE session SET spent = 1"
            "  WHERE nonce = ?1 AND driver = ?2 AND spent = 0 AND expires > ?3",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "spend session");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)nonce);
    sqlite3_bind_text(st, 2, who, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

int gs_store_forget_sessions(gs_store *s, int64_t before) {
    if (s == nullptr) return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "DELETE FROM session WHERE expires <= ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "forget sessions");
        return 0;
    }
    sqlite3_bind_int64(st, 1, before);
    int gone = sqlite3_step(st) == SQLITE_DONE ? sqlite3_changes(s->db) : 0;
    sqlite3_finalize(st);
    return gone;
}

int gs_store_session_count(gs_store *s) {
    return gs_count(s, "SELECT COUNT(*) FROM session");
}

bool gs_store_set_added(gs_store *s, uint64_t hash, int64_t when) {
    if (s == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "UPDATE track SET added = ?2 WHERE hash = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "set added");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_int64(st, 2, when);

    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) gs_fail(s, "set added");
    sqlite3_finalize(st);
    return ok;
}

int64_t gs_store_added(gs_store *s, uint64_t hash) {
    if (s == nullptr) return -1;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT added FROM track WHERE hash = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "added");
        return -1;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);

    int64_t when = -1;
    if (sqlite3_step(st) == SQLITE_ROW) when = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return when;
}

bool gs_store_has_track(gs_store *s, uint64_t hash) {
    return gs_store_get_track(s, hash, nullptr, 0, nullptr);
}

int gs_store_track_count(gs_store *s) {
    return gs_count(s, "SELECT COUNT(*) FROM track");
}

bool gs_store_publish(gs_store *s, uint64_t hash, const char *name,
                      const char *author) {
    if (s == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE track SET published = 1,"
            "  name = CASE WHEN ?2 <> '' THEN ?2 ELSE name END,"
            "  author = ?3"
            " WHERE hash = ?1",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "publish");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_text(st, 2, name != nullptr ? name : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, author != nullptr ? author : "", -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) > 0;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_withdraw(gs_store *s, uint64_t hash, const char *author) {
    if (s == nullptr) return false;

    // **Only by whoever put it up.** The track itself stays - times set on it
    // still have to be verifiable - it simply stops being listed.
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            // `published <> 0` matters: SQLite counts writing a value that is
            // already there as a change, so without it withdrawing something
            // already down reports success.
            "UPDATE track SET published = 0"
            " WHERE hash = ?1 AND author = ?2 AND published <> 0",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "withdraw");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_text(st, 2, author != nullptr ? author : "", -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) > 0;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_is_published(gs_store *s, uint64_t hash) {
    if (s == nullptr) return false;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "SELECT published FROM track WHERE hash = ?1", -1, &st,
            nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    bool yes = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) != 0;
    sqlite3_finalize(st);
    return yes;
}

int gs_store_list_published(gs_store *s, gs_track_row *out, int cap) {
    if (s == nullptr || out == nullptr || cap <= 0) return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "SELECT hash, name, author, LENGTH(bytes) FROM track"
            " WHERE published <> 0 ORDER BY added DESC, hash DESC LIMIT ?1",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "list published");
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);

    int n = 0;
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        gs_track_row *r = &out[n++];
        memset(r, 0, sizeof *r);
        r->hash = (uint64_t)sqlite3_column_int64(st, 0);
        const unsigned char *name = sqlite3_column_text(st, 1);
        const unsigned char *who = sqlite3_column_text(st, 2);
        snprintf(r->name, sizeof r->name, "%s", name != nullptr ? (const char *)name : "");
        snprintf(r->author, sizeof r->author, "%s", who != nullptr ? (const char *)who : "");
        r->bytes = (uint32_t)sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
    return n;
}

int gs_store_list_tracks(gs_store *s, gs_track_row *out, int cap) {
    if (s == nullptr || out == nullptr || cap <= 0) return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "SELECT hash, name, author, LENGTH(bytes) FROM track"
            " ORDER BY added DESC, hash DESC LIMIT ?1",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "list tracks");
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);

    int n = 0;
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        gs_track_row *r = &out[n++];
        memset(r, 0, sizeof *r);
        r->hash = (uint64_t)sqlite3_column_int64(st, 0);
        const unsigned char *name = sqlite3_column_text(st, 1);
        const unsigned char *who = sqlite3_column_text(st, 2);
        snprintf(r->name, sizeof r->name, "%s", name != nullptr ? (const char *)name : "");
        snprintf(r->author, sizeof r->author, "%s", who != nullptr ? (const char *)who : "");
        r->bytes = (uint32_t)sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
    return n;
}

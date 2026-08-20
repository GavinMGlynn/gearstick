#include "net/gs_store.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GS_SCHEMA 3

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

// **Add a column to a table that may already have it.**
//
// A fresh database gets the newest shape from the CREATE statements below and
// needs none of this. A database that already exists - the library committed
// under assets/, or a server that has been running for a month - has the old
// shape, and `CREATE TABLE IF NOT EXISTS` does exactly nothing to it. That is
// the whole failure mode this exists for: every query naming a new column would
// fail on the one database that matters and pass on every test that made its
// own.
//
// SQLite says "duplicate column name" when it is already there, which is the
// expected answer on a fresh database and is not an error.
static bool gs_add_column(gs_store *s, const char *table, const char *column) {
    char sql[256];
    snprintf(sql, sizeof sql, "ALTER TABLE %s ADD COLUMN %s", table, column);

    char *err = nullptr;
    if (sqlite3_exec(s->db, sql, nullptr, nullptr, &err) == SQLITE_OK) {
        sqlite3_free(err);
        return true;
    }
    bool already = err != nullptr && strstr(err, "duplicate column name") != nullptr;
    if (!already) {
        // The statement, then whatever SQLite said about it. Truncated rather
        // than assembled with a format the compiler has to prove fits.
        snprintf(s->error, sizeof s->error, "could not alter %s: %s", table,
                 err != nullptr ? err : "?");
    }
    sqlite3_free(err);
    return already;
}

static bool gs_migrate(gs_store *s) {
    // The schema, and the version it is. A fresh database is created at the
    // newest shape; an older one is brought up to it by the alterations after
    // the creates.
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
        "  seen    INTEGER NOT NULL DEFAULT 0,"

        // **What makes a name yours rather than one you typed.**
        //
        // Null for a driver with no password, which is most of them and has to
        // keep working: a racing game that demands an account before anybody
        // can drive has lost the argument. Set, and the name cannot be used
        // without it.
        //
        // The hash is libsodium's `crypto_pwhash_str` - Argon2id, with the
        // salt and the parameters inside the string, so there is nothing here
        // to get wrong and nothing to store alongside it.
        "  password TEXT,"

        // A shared secret for a one-time code, for anybody who wants one, and
        // the last counter accepted under it. **The counter is why a code
        // cannot be used twice** inside the thirty seconds it stays valid -
        // without it, somebody who saw a code has that long to use it.
        "  totp     BLOB,"
        "  totp_at  INTEGER NOT NULL DEFAULT 0);"

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

        // **Whose track this is, as a key rather than a name.**
        //
        // It used to be the author string, which is whatever the uploader
        // typed - so "only the person who put it up may take it down" meant
        // "only somebody willing to type the same word". The owner is now the
        // static public key the client proved it holds during its handshake,
        // which is a thing nobody else can present.
        //
        // Null for a track that shipped with the game. That is not an owner
        // nobody has got round to setting: a stock track is outside ownership
        // altogether and no request may touch it, which `shipped` says out loud
        // rather than leaving to be inferred from a null.
        "  owner     BLOB,"
        "  shipped   INTEGER NOT NULL DEFAULT 0,"

        // 0 private, 1 shared with named people, 2 published to everybody.
        "  visible   INTEGER NOT NULL DEFAULT 0,"
        "  bytes     BLOB NOT NULL);"

        // Who a shared track is shared with, one row each.
        "CREATE TABLE IF NOT EXISTS track_share ("
        "  hash   INTEGER NOT NULL,"
        "  viewer BLOB NOT NULL,"
        "  PRIMARY KEY (hash, viewer));"

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

        "CREATE INDEX IF NOT EXISTS session_by_driver ON session (driver);"

        // **Who this server is.** One row, and the check constraint says so
        // rather than leaving it to whoever writes the next insert. A server
        // with two identities is a server that answers to whichever one it
        // happened to read first.
        "CREATE TABLE IF NOT EXISTS identity ("
        "  id     INTEGER PRIMARY KEY CHECK (id = 1),"
        "  secret BLOB NOT NULL);")) {
        return false;
    }

    // Anything a database made before these columns existed is missing. Each is
    // added with the default that makes an old row mean what it always meant: a
    // track nobody owns, that did not ship, and that is published if the old
    // `published` column said so - which is set below.
    if (!gs_add_column(s, "driver", "password TEXT") ||
        !gs_add_column(s, "driver", "totp BLOB") ||
        !gs_add_column(s, "driver", "totp_at INTEGER NOT NULL DEFAULT 0") ||
        !gs_add_column(s, "track", "owner BLOB") ||
        !gs_add_column(s, "track", "shipped INTEGER NOT NULL DEFAULT 0") ||
        !gs_add_column(s, "track", "visible INTEGER NOT NULL DEFAULT 0")) {
        return false;
    }

    // **The old `published` flag is the new visibility.** A track that was up
    // before this change stays up; one that was not stays private. Doing it here
    // rather than leaving the two to disagree is the point of a migration.
    if (!gs_exec(s, "UPDATE track SET visible = 2 WHERE published != 0 AND visible = 0;")) {
        return false;
    }

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO meta (key, value) VALUES ('schema', ?1)"
            "  ON CONFLICT(key) DO UPDATE SET value = excluded.value",
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

// --- proving a name is yours ------------------------------------------------

// Every one of these is about a driver row that may not exist yet, so the
// driver is made first. A password set on a name nobody has used is a name
// somebody has reserved, which is the honest reading of it.
static bool gs_driver_row(gs_store *s, const char *name) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO driver (name) VALUES (?1) ON CONFLICT(name) DO NOTHING",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "driver row");
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_set_password(gs_store *s, const char *name, const char *hash) {
    if (s == nullptr || name == nullptr || name[0] == '\0') return false;
    if (!gs_driver_row(s, name)) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "UPDATE driver SET password = ?2 WHERE name = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "set password");
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (hash != nullptr) {
        sqlite3_bind_text(st, 2, hash, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 2);
    }
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_password(gs_store *s, const char *name, char *hash, size_t cap) {
    if (s == nullptr || name == nullptr || hash == nullptr || cap == 0) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT password FROM driver WHERE name = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "password");
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);

    bool got = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(st, 0);
        if (text != nullptr) {
            snprintf(hash, cap, "%s", (const char *)text);
            got = hash[0] != '\0';
        }
    }
    sqlite3_finalize(st);
    return got;
}

bool gs_store_has_password(gs_store *s, const char *name) {
    char hash[GS_STORE_PWHASH];
    return gs_store_password(s, name, hash, sizeof hash);
}

bool gs_store_set_totp(gs_store *s, const char *name, const uint8_t *secret,
                       size_t len) {
    if (s == nullptr || name == nullptr || name[0] == '\0') return false;
    if (len > GS_STORE_TOTP) return false;
    if (!gs_driver_row(s, name)) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE driver SET totp = ?2, totp_at = 0 WHERE name = ?1",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "set totp");
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (secret != nullptr && len > 0) {
        sqlite3_bind_blob(st, 2, secret, (int)len, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 2);
    }
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_totp(gs_store *s, const char *name, uint8_t *secret, size_t cap,
                   size_t *len) {
    if (s == nullptr || name == nullptr || secret == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT totp FROM driver WHERE name = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "totp");
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);

    bool got = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (blob != nullptr && n > 0 && (size_t)n <= cap) {
            memcpy(secret, blob, (size_t)n);
            if (len != nullptr) *len = (size_t)n;
            got = true;
        }
    }
    sqlite3_finalize(st);
    return got;
}

bool gs_store_totp_use(gs_store *s, const char *name, int64_t counter) {
    if (s == nullptr || name == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE driver SET totp_at = ?2 WHERE name = ?1 AND totp_at < ?2",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "use totp");
        return false;
    }
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, counter);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

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

// --- identity ---------------------------------------------------------------

bool gs_store_identity(gs_store *s, uint8_t *secret) {
    if (s == nullptr || secret == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT secret FROM identity WHERE id = 1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "read identity");
        return false;
    }

    bool got = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);

        // A key of the wrong length is not a key. Refusing it makes the server
        // mint a new one, which is better than handing a truncated secret to a
        // handshake and finding out later.
        if (blob != nullptr && n == GS_STORE_IDENTITY_BYTES) {
            memcpy(secret, blob, GS_STORE_IDENTITY_BYTES);
            got = true;
        }
    }
    sqlite3_finalize(st);
    return got;
}

bool gs_store_set_identity(gs_store *s, const uint8_t *secret) {
    if (s == nullptr || secret == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO identity (id, secret) VALUES (1, ?1)"
            "  ON CONFLICT(id) DO UPDATE SET secret = excluded.secret",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "write identity");
        return false;
    }
    sqlite3_bind_blob(st, 1, secret, GS_STORE_IDENTITY_BYTES, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) gs_fail(s, "write identity");
    sqlite3_finalize(st);
    return ok;
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

// --- who a track belongs to -------------------------------------------------

// The one question every write path asks first: is this person allowed to
// change this track? Answered in SQL so that the check and the change can be
// the same statement, and there is no gap between them.
//
// The condition reads: the track exists, it did not ship with the game, and its
// owner is exactly this key. A shipped track fails the second clause whoever is
// asking, which is the rule stated once here instead of in five places.
#define GS_OWNED_BY \
    " WHERE hash = ?1 AND shipped = 0 AND owner IS NOT NULL AND owner = ?2"

bool gs_store_claim_track(gs_store *s, uint64_t hash, const uint8_t *owner) {
    if (s == nullptr || owner == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE track SET owner = ?2"
            "  WHERE hash = ?1 AND shipped = 0 AND (owner IS NULL OR owner = ?2)",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "claim track");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_blob(st, 2, owner, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_track_owner(gs_store *s, uint64_t hash, uint8_t *owner) {
    if (s == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT owner FROM track WHERE hash = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "track owner");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);

    bool got = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(st, 0);
        int n = sqlite3_column_bytes(st, 0);
        if (blob != nullptr && n == GS_STORE_KEY_BYTES) {
            if (owner != nullptr) memcpy(owner, blob, GS_STORE_KEY_BYTES);
            got = true;
        }
    }
    sqlite3_finalize(st);
    return got;
}

bool gs_store_track_is_shipped(gs_store *s, uint64_t hash) {
    if (s == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT shipped FROM track WHERE hash = ?1",
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "shipped");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    bool yes = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) != 0;
    sqlite3_finalize(st);
    return yes;
}

bool gs_store_mark_shipped(gs_store *s, uint64_t hash) {
    if (s == nullptr) return false;

    // Shipped and ownerless in one statement: a stock track with an owner would
    // be a stock track somebody could take down.
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            // Published as well as visible: a track that shipped with the
            // game is up by definition, and leaving the two to disagree would
            // mean a stock track that is listed by one query and not the other.
            "UPDATE track SET shipped = 1, owner = NULL, visible = 2,"
            "                 published = 1"
            "  WHERE hash = ?1",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "mark shipped");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_name_track(gs_store *s, uint64_t hash, const uint8_t *who,
                         const char *name) {
    if (s == nullptr || who == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "UPDATE track SET name = ?3" GS_OWNED_BY,
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "name track");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_blob(st, 2, who, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, name != nullptr ? name : "", -1, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_set_visible(gs_store *s, uint64_t hash, const uint8_t *who,
                          gs_visible how) {
    if (s == nullptr || who == nullptr) return false;
    if (how != GS_TRACK_PRIVATE && how != GS_TRACK_SHARED &&
        how != GS_TRACK_PUBLIC) {
        return false;
    }

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "UPDATE track SET visible = ?3, published = (?3 = 2)" GS_OWNED_BY,
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "set visibility");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_blob(st, 2, who, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, (int)how);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    return ok;
}

// Sharing is two statements that must not come apart: the owner check, and the
// row. Wrapped so that a share never lands against a track the asker does not
// own even if the two race.
static bool gs_owns(gs_store *s, uint64_t hash, const uint8_t *who) {
    if (who == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "SELECT 1 FROM track" GS_OWNED_BY,
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "owner check");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_blob(st, 2, who, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);
    bool yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return yes;
}

bool gs_store_share_track(gs_store *s, uint64_t hash, const uint8_t *who,
                          const uint8_t *with) {
    if (s == nullptr || with == nullptr || !gs_owns(s, hash, who)) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO track_share (hash, viewer) VALUES (?1, ?2)"
            "  ON CONFLICT(hash, viewer) DO NOTHING",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "share track");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_blob(st, 2, with, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_unshare_track(gs_store *s, uint64_t hash, const uint8_t *who,
                            const uint8_t *with) {
    if (s == nullptr || with == nullptr || !gs_owns(s, hash, who)) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "DELETE FROM track_share WHERE hash = ?1 AND viewer = ?2",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "unshare track");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_blob(st, 2, with, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool gs_store_delete_track(gs_store *s, uint64_t hash, const uint8_t *who) {
    if (s == nullptr || who == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db, "DELETE FROM track" GS_OWNED_BY,
                           -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "delete track");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    sqlite3_bind_blob(st, 2, who, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);

    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(s->db) == 1;
    sqlite3_finalize(st);
    if (!ok) return false;

    // The shares go with it. Leaving them would mean somebody re-uploading the
    // same track inherited an audience they never chose.
    sqlite3_stmt *tidy = nullptr;
    if (sqlite3_prepare_v2(s->db, "DELETE FROM track_share WHERE hash = ?1",
                           -1, &tidy, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(tidy, 1, (sqlite3_int64)hash);
        sqlite3_step(tidy);
        sqlite3_finalize(tidy);
    }
    return true;
}

// The one condition that decides who may see what, written once. A null viewer
// binds as SQL NULL, which never equals anything - so somebody asserting no
// identity sees the shipped and the public and nothing else.
#define GS_VISIBLE_TO \
    " (t.shipped != 0 OR t.visible = 2"                                        \
    "  OR t.owner = ?2"                                                        \
    "  OR (t.visible = 1 AND EXISTS ("                                         \
    "        SELECT 1 FROM track_share sh"                                     \
    "         WHERE sh.hash = t.hash AND sh.viewer = ?2))) "

bool gs_store_can_see(gs_store *s, uint64_t hash, const uint8_t *who) {
    if (s == nullptr) return false;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "SELECT 1 FROM track t WHERE t.hash = ?1 AND" GS_VISIBLE_TO,
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "can see");
        return false;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)hash);
    if (who != nullptr) {
        sqlite3_bind_blob(st, 2, who, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 2);
    }
    bool yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return yes;
}

int gs_store_list_visible(gs_store *s, const uint8_t *who, gs_track_row *out,
                          int cap) {
    if (s == nullptr || out == nullptr || cap <= 0) return 0;

    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(s->db,
            "SELECT t.hash, t.name, t.author, LENGTH(t.bytes) FROM track t"
            " WHERE" GS_VISIBLE_TO
            " ORDER BY t.added DESC, t.hash DESC LIMIT ?1",
            -1, &st, nullptr) != SQLITE_OK) {
        gs_fail(s, "list visible");
        return 0;
    }
    sqlite3_bind_int(st, 1, cap);
    if (who != nullptr) {
        sqlite3_bind_blob(st, 2, who, GS_STORE_KEY_BYTES, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(st, 2);
    }

    int n = 0;
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        gs_track_row *r = &out[n++];
        memset(r, 0, sizeof *r);
        r->hash = (uint64_t)sqlite3_column_int64(st, 0);
        const unsigned char *name = sqlite3_column_text(st, 1);
        const unsigned char *author = sqlite3_column_text(st, 2);
        snprintf(r->name, sizeof r->name, "%s",
                 name != nullptr ? (const char *)name : "");
        snprintf(r->author, sizeof r->author, "%s",
                 author != nullptr ? (const char *)author : "");
        r->bytes = (uint32_t)sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
    return n;
}

bool gs_store_publish(gs_store *s, uint64_t hash, const char *name,
                      const char *author) {
    // Kept for the tool that builds the shipped library, which has no keys and
    // no clients - see gs_store_mark_shipped, which is what makes those tracks
    // untouchable afterwards. A client never reaches this: everything it can do
    // to a track goes through the owner check.
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
            // **And never a track that shipped with the game.** The author
            // string is whatever somebody typed, and the stock library's says
            // "gearstick" - so without `shipped = 0` a profile called that
            // could take down the tracks the game came with. That is exactly
            // the case this rule exists for.
            "UPDATE track SET published = 0, visible = 0"
            " WHERE hash = ?1 AND author = ?2 AND published <> 0 AND shipped = 0",
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

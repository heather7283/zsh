#include "sqlite_history.mdh"
#include "sqlite_history.pro"

#include <sqlite3.h>

#define DB_VERSION 1

static struct {
    sqlite3 *db;
    sqlite3_stmt *insert;
    long session_id;
} g = {
    .db = NULL,
    .insert = NULL,
    .session_id = LONG_MIN,
};

/**/
static bool
exec(const char *sql)
{
    int ret = sqlite3_exec(g.db, sql, NULL, NULL, NULL);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "failed to execute sql: %s\n", sqlite3_errmsg(g.db));
        fprintf(stderr, "source: %s\n", sql);
        return false;
    }

    return true;
}

/**/
static struct sqlite3_stmt *
prepare(const char *sql)
{
    struct sqlite3_stmt* stmt = NULL;
    int ret = sqlite3_prepare_v2(g.db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "failed to prepare stmt: %s\n", sqlite3_errmsg(g.db));
        fprintf(stderr, "source: %s\n", sql);
        return NULL;
    }

    return stmt;
}

/**/
static bool
get_user_version(int *version)
{
    int res;

    struct sqlite3_stmt *stmt = prepare("PRAGMA user_version;");
    if (!stmt) {
        return false;
    }

    int ret = sqlite3_step(stmt);
    res = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (ret != SQLITE_ROW) {
        fprintf(stderr, "could not get user_version: %s\n",
                sqlite3_errstr(ret));
        return false;
    } else {
        *version = res;
        return true;
    }
}

/**/
static bool
set_user_version(int version)
{
    /* PRAGMA doesn't support parameters :/ */
    char buf[35];
    snprintf(buf, sizeof(buf), "PRAGMA user_version = %d;", version);

    return exec(buf);
}

/**/
static bool
save_histent(const struct histent *he)
{
    bool ret = true;

    sqlite3_bind_int64(g.insert,
                       sqlite3_bind_parameter_index(g.insert, "@session_id"),
                       g.session_id);
    sqlite3_bind_text(g.insert,
                      sqlite3_bind_parameter_index(g.insert, "@command"),
                      he->node.nam, -1, SQLITE_STATIC);
    sqlite3_bind_int64(g.insert,
                       sqlite3_bind_parameter_index(g.insert, "@started_at"),
                       he->stim);
    sqlite3_bind_int64(g.insert,
                       sqlite3_bind_parameter_index(g.insert, "@finished_at"),
                       he->ftim);

    if (sqlite3_step(g.insert) != SQLITE_DONE) {
        fprintf(stderr, "could not insert entry: %s\n", sqlite3_errmsg(g.db));
        ret = false;
    }

    sqlite3_clear_bindings(g.insert);
    sqlite3_reset(g.insert);

    return ret;
}

/**/
static bool
start_session(long *id)
{
    static const char sql[] =
        "INSERT INTO sessions ( created_at ) VALUES ( unixepoch() ) RETURNING id;"
    ;

    bool ret = true;
    struct sqlite3_stmt *stmt = NULL;

    stmt = prepare(sql);
    if (!stmt) {
        ret = false;
        goto out;
    }

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        fprintf(stderr, "could not get shell session id: %s\n",
                sqlite3_errmsg(g.db));
        ret = false;
        goto out;
    }

    *id = sqlite3_column_int64(stmt, 0);

out:
    sqlite3_finalize(stmt);
    return ret;
}

/**/
static bool
migrate(bool verbose)
{
    int version;
    if (!get_user_version(&version)) {
        return false;
    }

    static const char *migrations[DB_VERSION] = {
        /* db version 1 - initial schema */
        "CREATE TABLE sessions (\n"
        "    id         INTEGER PRIMARY KEY,\n"
        "    created_at INTEGER NOT NULL\n"
        ");\n"
        "CREATE TABLE commands (\n"
        "    id          INTEGER PRIMARY KEY,\n"
        "    session_id  INTEGER NOT NULL,\n"
        "\n"
        "    command     TEXT    NOT NULL,\n"
        "    started_at  INTEGER NOT NULL,\n"
        "    finished_at INTEGER NOT NULL,\n"
        "\n"
        "    FOREIGN KEY ( session_id ) REFERENCES sessions ( id ) ON DELETE CASCADE\n"
        ");\n"
    };

    while (version != DB_VERSION) {
        if (verbose) {
            printf("sqlite_history: migration %d -> %d\n",
                   version, version + 1);
        }

        if (!exec(migrations[version]) || !set_user_version(++version)) {
            return false;
        }
    }

    return true;
}

/**/
static int
bin_sqlite_history_open(char *nam, char **args, Options ops, UNUSED(int func))
{
    const bool verbose = OPT_ISSET(ops, 'v');

    if (g.db) {
        fprintf(stderr, "database already opened\n");
        return 1;
    }

    int ret = sqlite3_open(args[0], &g.db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "could not open g.db: %s\n", sqlite3_errstr(ret));
        goto err;
    }

    if (verbose) {
        printf("sqlite_history: opened g.db at %s\n", args[0]);
    }

    if (!migrate(verbose)) {
        fprintf(stderr, "migration failed\n");
        goto err;
    }

    if (!start_session(&g.session_id)) {
        goto err;
    }


    static const char insert_source[] =
        "INSERT INTO commands ( session_id, command, started_at, finished_at )"
        " VALUES ( @session_id, @command, @started_at, @finished_at );"
    ;
    g.insert = prepare(insert_source);
    if (!g.insert) {
        goto err;
    }

    return 0;

err:
    sqlite3_finalize(g.insert);
    g.insert = NULL;

    sqlite3_close(g.db);
    g.db = NULL;

    g.session_id = LONG_MIN;

    return 1;
}

/**/
static int
bin_sqlite_history_close(char *nam, char **args, Options ops, UNUSED(int func))
{
    const bool verbose = OPT_ISSET(ops, 'v');

    if (!g.db) {
        fprintf(stderr, "database is not opened\n");
        return 1;
    }

    int ret = sqlite3_close(g.db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "could not close g.db: %s\n", sqlite3_errstr(ret));
        return 1;
    }

    if (verbose) {
        printf("sqlite_history: closed g.db\n");
    }
    g.db = NULL;

    return 0;
}

/**/
static int
bin_sqlite_history_save(char *nam, char **args, Options ops, UNUSED(int func))
{
    const bool verbose = OPT_ISSET(ops, 'v');

    struct histent *he = hist_ring->up;
    if (!he) {
        fprintf(stderr, "no last history entry found\n");
        return 1;
    }

    if (verbose) {
        printf("sqlite_history: last item: stim=%lu ftim=%lu text=%s\n",
               he->stim, he->ftim, he->node.nam);
    }

    if (!save_histent(he)) {
        return 1;
    }

    return 0;
}

static struct builtin bintab[] = {
    BUILTIN("sqlite_history_open", 0, bin_sqlite_history_open, 1, 1, 0, "v", NULL),
    BUILTIN("sqlite_history_close", 0, bin_sqlite_history_close, 0, 0, 0, "v", NULL),
    BUILTIN("sqlite_history_save", 0, bin_sqlite_history_save, 0, 0, 0, "v", NULL),
};

static struct features module_features = {
    bintab, sizeof(bintab)/sizeof(*bintab),
    NULL, 0,
    NULL, 0,
    NULL, 0,
    0
};

/**/
int
setup_(UNUSED(Module m))
{
    return 0;
}

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}

/**/
int
boot_(Module m)
{
    return 0;
}

/**/
int
cleanup_(Module m)
{
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(UNUSED(Module m))
{
    sqlite3_finalize(g.insert);
    g.insert = NULL;

    int ret = sqlite3_close(g.db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "sqlite_history: close g.db: %s\n", sqlite3_errstr(ret));
    }
    g.db = NULL;

    return 0;
}


#include "sqlite_history.mdh"
#include "sqlite_history.pro"

#include <sqlite3.h>

#define DB_VERSION 1

static sqlite3 *db = NULL;

/**/
static bool
exec(const char *sql)
{
    int ret = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "failed to execute sql: %s\n", sqlite3_errmsg(db));
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
    int ret = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "failed to prepare stmt: %s\n", sqlite3_errmsg(db));
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
migrate(bool verbose)
{
    int version;
    if (!get_user_version(&version)) {
        return false;
    }

    static const char *migrations[DB_VERSION] = {
        /* db version 1 - initial schema */
        "CREATE TABLE history (\n"
        "    id          INTEGER PRIMARY KEY,\n"
        "    command     TEXT    NOT NULL,\n"
        "    start_time  INTEGER NOT NULL,\n"
        "    finish_time INTEGER NOT NULL\n"
        ");",
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

    if (db) {
        fprintf(stderr, "database already opened\n");
        return 1;
    }

    int ret = sqlite3_open(args[0], &db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "could not open db: %s\n", sqlite3_errstr(ret));
        goto err;
    }

    if (verbose) {
        printf("sqlite_history: opened db at %s\n", args[0]);
    }

    if (!migrate(verbose)) {
        fprintf(stderr, "migration failed\n");
        goto err;
    }

    return 0;

err:
    sqlite3_close(db);
    db = NULL;
    return 1;
}

/**/
static int
bin_sqlite_history_close(char *nam, char **args, Options ops, UNUSED(int func))
{
    const bool verbose = OPT_ISSET(ops, 'v');

    if (!db) {
        fprintf(stderr, "database is not opened\n");
        return 1;
    }

    int ret = sqlite3_close(db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "could not close db: %s\n", sqlite3_errstr(ret));
        return 1;
    }

    if (verbose) {
        printf("sqlite_history: closed db\n");
    }
    db = NULL;

    return 0;
}

static struct builtin bintab[] = {
    BUILTIN("sqlite_history_open", 0, bin_sqlite_history_open, 1, 1, 0, "v", NULL),
    BUILTIN("sqlite_history_close", 0, bin_sqlite_history_close, 0, 0, 0, "v", NULL),
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
    printf("sqlite_history: running with sqlite ver %s\n", sqlite3_libversion());
    fflush(stdout);
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
    int ret = sqlite3_close(db);
    if (ret != SQLITE_OK) {
        fprintf(stderr, "sqlite_history: close db: %s\n", sqlite3_errstr(ret));
    }
    db = NULL;

    return 0;
}


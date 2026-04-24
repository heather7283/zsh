#include "sqlite_history.mdh"
#include "sqlite_history.pro"

#include <sqlite3.h>

#define INFO(msg, ...) \
    do { \
        if (g.verbose) printf("sqlite_history: " msg "\n", ##__VA_ARGS__); \
    } while (0)
#define ERROR(msg, ...) \
    fprintf(stderr, "sqlite_history: " msg "\n", ##__VA_ARGS__)

#define DB_VERSION 1

static struct {
    sqlite3 *db;
    sqlite3_stmt *insert;
    long session_id;
    bool verbose;
} g = {
    .db = NULL,
    .insert = NULL,
    .session_id = LONG_MIN,
    .verbose = false,
};

/**/
static bool
exec(const char *sql)
{
    int ret = sqlite3_exec(g.db, sql, NULL, NULL, NULL);
    if (ret != SQLITE_OK) {
        ERROR("failed to execute sql: %s", sqlite3_errmsg(g.db));
        ERROR("source: %s", sql);
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
        ERROR("failed to prepare stmt: %s", sqlite3_errmsg(g.db));
        ERROR("source: %s", sql);
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
        ERROR("could not get user_version: %s",
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
                      unmeta(he->node.nam), -1, SQLITE_STATIC);
    sqlite3_bind_int64(g.insert,
                       sqlite3_bind_parameter_index(g.insert, "@started_at"),
                       he->stim);
    sqlite3_bind_int64(g.insert,
                       sqlite3_bind_parameter_index(g.insert, "@finished_at"),
                       he->ftim);

    if (sqlite3_step(g.insert) != SQLITE_DONE) {
        ERROR("could not insert entry: %s", sqlite3_errmsg(g.db));
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
        ERROR("could not get shell session id: %s", sqlite3_errmsg(g.db));
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
migrate(void)
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
        "    command     TEXT    NOT NULL CHECK ( length(command) > 0 ),\n"
        "    started_at  INTEGER NOT NULL CHECK ( started_at > 0 ),\n"
        "    finished_at INTEGER NOT NULL CHECK ( finished_at >= started_at ),\n"
        "\n"
        "    FOREIGN KEY ( session_id ) REFERENCES sessions ( id ) ON DELETE CASCADE\n"
        ");\n"
    };

    if (version > DB_VERSION) {
        ERROR("db has version %d while this zsh build only supports up to %d",
              version, DB_VERSION);
        return false;
    } else while (version < DB_VERSION) {
        INFO("sqlite_history: migration %d -> %d", version, version + 1);
        if (!exec(migrations[version]) || !set_user_version(++version)) {
            return false;
        }
    }

    return true;
}

/**/
static int
sqlite_history_open(char *nam, char **args, Options ops, UNUSED(int func))
{
    if (g.db) {
        ERROR("database already opened");
        return 1;
    }

    g.verbose = OPT_ISSET(ops, 'v');

    int ret = sqlite3_open(args[0], &g.db);
    if (ret != SQLITE_OK) {
        ERROR("could not open db: %s", sqlite3_errstr(ret));
        goto err;
    }
    INFO("opened db at %s", args[0]);

    if (!exec("PRAGMA foreign_keys = 1;") || !exec("PRAGMA journal_mode = WAL;")) {
        goto err;
    }

    if (!migrate()) {
        ERROR("migration failed");
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
    g.verbose = false;

    return 1;
}

/**/
static int
sqlite_history_close(char *nam, char **args, Options ops, UNUSED(int func))
{
    if (!g.db) {
        ERROR("database is not opened");
        return 1;
    }

    sqlite3_finalize(g.insert);
    g.insert = NULL;

    int ret = sqlite3_close(g.db);
    if (ret != SQLITE_OK) {
        ERROR("close db: %s", sqlite3_errstr(ret));
    }
    g.db = NULL;

    g.session_id = LONG_MIN;
    g.verbose = false;

    return 0;
}

/**/
static int
sqlite_history_save(char *nam, char **args, Options ops, UNUSED(int func))
{
    struct histent *he = hist_ring->up;
    if (!he) {
        ERROR("no last history entry found");
        return 1;
    } else if (he->node.nam[0] == '\0') {
        /* do not process empty entries */
        return 0;
    }

    INFO("sqlite_history: last item: stim=%lu ftim=%lu text=%s",
         he->stim, he->ftim, he->node.nam);

    if (!save_histent(he)) {
        return 1;
    }

    return 0;
}

/**/
static int
sqlite_history_list(char *nam, char **args, Options ops, UNUSED(int func))
{
    const char sep = OPT_ISSET(ops, 'z') ? '\0' : '\n';
    const char *const order = OPT_ISSET(ops, 'r') ? "DESC" : "ASC";
    int ret = 0;
    struct sqlite3_stmt *stmt = NULL;

    char buf[55];
    snprintf(buf, sizeof(buf),
             "SELECT command FROM commands ORDER BY started_at %s;", order);

    stmt = prepare(buf);
    if (!stmt) {
        ret = 1;
        goto out;
    }

    while (true) {
        const char *cmd;
        size_t len;

        switch (sqlite3_step(stmt)) {
        case SQLITE_ROW:
            cmd = (const char *) sqlite3_column_text(stmt, 0);
            len = sqlite3_column_bytes(stmt, 0);
            fwrite(cmd, 1, len, stdout);
            fwrite(&sep, 1, 1, stdout);
            continue;
        case SQLITE_DONE:
            goto out;
        default:
            ERROR("could not list history entries: %s", sqlite3_errmsg(g.db));
            ret = 1;
            goto out;
        }
    }

out:
    sqlite3_finalize(stmt);
    fflush(stdout);
    return ret;
}

static struct builtin bintab[] = {
    BUILTIN("sqlite_history_open", 0, sqlite_history_open, 1, 1, 0, "v", NULL),
    BUILTIN("sqlite_history_close", 0, sqlite_history_close, 0, 0, 0, "", NULL),
    BUILTIN("sqlite_history_save", 0, sqlite_history_save, 0, 0, 0, "", NULL),
    BUILTIN("sqlite_history_list", 0, sqlite_history_list, 0, 0, 0, "zr", NULL),
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
        ERROR("close db: %s", sqlite3_errstr(ret));
    }
    g.db = NULL;

    g.session_id = LONG_MIN;
    g.verbose = false;

    return 0;
}

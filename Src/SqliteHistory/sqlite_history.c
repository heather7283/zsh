#include "sqlite_history.mdh"
#include "sqlite_history.pro"

#include <dlfcn.h>

#define INFO(msg, ...) \
    do { \
        if (g.verbose) printf("sqlite-history: " msg "\n", ##__VA_ARGS__); \
    } while (0)

#define ERROR(msg, ...) \
    fprintf(stderr, "sqlite-history: " msg "\n", ##__VA_ARGS__)

#define countof(arr) (sizeof(arr) / sizeof(arr[0]))

#define DB_VERSION 3


#define SQLITE_OK           0   /* Successful result */
#define SQLITE_DONE        101  /* sqlite3_step() has finished executing */
#define SQLITE_ROW         100  /* sqlite3_step() has another row ready */

typedef void (*sqlite3_destructor_type)(void*);
#define SQLITE_STATIC      ((sqlite3_destructor_type)0)

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

#define FOR_LIST_OF_FUNCTIONS(DO) \
    DO(int, exec, sqlite3 *db, const char *sql, \
	          int (*callback)(void *data, int ncols, char **values, char **names), void *data, \
	          char **errmsg) \
    DO(const char *, errmsg, sqlite3 *db) \
    DO(const char *, errstr, int code) \
    DO(int, prepare_v2, sqlite3 *db, const char *zSql, int nByte, \
                        sqlite3_stmt **ppStmt, const char **pzTail) \
    DO(int, step, sqlite3_stmt *pStmt) \
    DO(int, column_int, sqlite3_stmt *pStmt, int iCol) \
    DO(int64_t, column_int64, sqlite3_stmt *pStmt, int iCol) \
    DO(const unsigned char *, column_text, sqlite3_stmt *pStmt, int iCol) \
    DO(int, column_bytes, sqlite3_stmt *pStmt, int iCol) \
    DO(int, finalize, sqlite3_stmt *pStmt) \
    DO(int, bind_parameter_index, sqlite3_stmt *pStmt, const char *zName) \
    DO(int, bind_int64, sqlite3_stmt *pStmt, int iCol, int64_t val) \
    DO(int, bind_text, sqlite3_stmt *pStmt, int iCol, \
                       const char *str, int len, \
                       void (*cleanup)(void *)) \
    DO(int, clear_bindings, sqlite3_stmt *pStmt) \
    DO(int, reset, sqlite3_stmt *pStmt) \
    DO(int, open, const char *filename, sqlite3 **ppDb) \
    DO(int, close, sqlite3 *pDb)

#define DEFINE_FUNCTION_NAMES(ret, name, ...) "sqlite3_"#name,
static const char *const sqlite_function_names[] = {
    FOR_LIST_OF_FUNCTIONS(DEFINE_FUNCTION_NAMES)
};

#define DEFINE_FUNCTION_STRUCT_ELEMENT(ret, name, ...) ret (*name)(__VA_ARGS__);
static struct {
    FOR_LIST_OF_FUNCTIONS(DEFINE_FUNCTION_STRUCT_ELEMENT)
} sqlite = {0};

static void *sqlite_handle = NULL;


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
    int ret = sqlite.exec(g.db, sql, NULL, NULL, NULL);
    if (ret != SQLITE_OK) {
        ERROR("failed to execute sql: %s", sqlite.errmsg(g.db));
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
    int ret = sqlite.prepare_v2(g.db, sql, -1, &stmt, NULL);
    if (ret != SQLITE_OK) {
        ERROR("failed to prepare stmt: %s", sqlite.errmsg(g.db));
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

    int ret = sqlite.step(stmt);
    res = sqlite.column_int(stmt, 0);
    sqlite.finalize(stmt);

    if (ret != SQLITE_ROW) {
        ERROR("could not get user_version: %s", sqlite.errstr(ret));
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

    sqlite.bind_int64(g.insert,
                      sqlite.bind_parameter_index(g.insert, "@session_id"),
                      g.session_id);
    sqlite.bind_text(g.insert,
                     sqlite.bind_parameter_index(g.insert, "@command"),
                     unmeta(he->node.nam), -1, SQLITE_STATIC);
    sqlite.bind_int64(g.insert,
                      sqlite.bind_parameter_index(g.insert, "@started_at"),
                      he->stim);
    sqlite.bind_int64(g.insert,
                      sqlite.bind_parameter_index(g.insert, "@finished_at"),
                      he->ftim);

    if (sqlite.step(g.insert) != SQLITE_DONE) {
        ERROR("could not insert entry: %s", sqlite.errmsg(g.db));
        ret = false;
    }

    sqlite.clear_bindings(g.insert);
    sqlite.reset(g.insert);

    return ret;
}

/**/
static bool
start_session(long *id)
{
    static const char sql[] =
        "INSERT INTO sessions ( started_at ) VALUES ( unixepoch() ) RETURNING id;"
    ;

    bool ret = true;
    struct sqlite3_stmt *stmt = NULL;

    stmt = prepare(sql);
    if (!stmt) {
        ret = false;
        goto out;
    }

    if (sqlite.step(stmt) != SQLITE_ROW) {
        ERROR("could not get shell session id: %s", sqlite.errmsg(g.db));
        ret = false;
        goto out;
    }

    *id = sqlite.column_int64(stmt, 0);

out:
    sqlite.finalize(stmt);
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
        ");\n",
        /* db version 2 - add index on started_at for faster listing */
        "CREATE INDEX idx_commands_started_at ON commands ( started_at );\n",
	/* db version 3 - rename created_at to started_at */
	"ALTER TABLE sessions RENAME COLUMN created_at TO started_at;\n",
    };

    if (version > DB_VERSION) {
        ERROR("db has version %d while this zsh build only supports up to %d",
              version, DB_VERSION);
        return false;
    } else while (version < DB_VERSION) {
        INFO("sqlite-history: migration %d -> %d", version, version + 1);
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

    int ret = sqlite.open(args[0], &g.db);
    if (ret != SQLITE_OK) {
        ERROR("could not open db: %s", sqlite.errstr(ret));
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
    sqlite.finalize(g.insert);
    g.insert = NULL;

    sqlite.close(g.db);
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

    sqlite.finalize(g.insert);
    g.insert = NULL;

    int ret = sqlite.close(g.db);
    if (ret != SQLITE_OK) {
        ERROR("close db: %s", sqlite.errstr(ret));
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

    INFO("sqlite-history: last item: stim=%lu ftim=%lu text=%s",
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

        switch (sqlite.step(stmt)) {
        case SQLITE_ROW:
            cmd = (const char *)sqlite.column_text(stmt, 0);
            len = sqlite.column_bytes(stmt, 0);
            fwrite(cmd, 1, len, stdout);
            fwrite(&sep, 1, 1, stdout);
            continue;
        case SQLITE_DONE:
            goto out;
        default:
            ERROR("could not list history entries: %s", sqlite.errmsg(g.db));
            ret = 1;
            goto out;
        }
    }

out:
    sqlite.finalize(stmt);
    fflush(stdout);
    return ret;
}

static struct builtin bintab[] = {
    BUILTIN("sqlite-history-open", 0, sqlite_history_open, 1, 1, 0, "v", NULL),
    BUILTIN("sqlite-history-close", 0, sqlite_history_close, 0, 0, 0, "", NULL),
    BUILTIN("sqlite-history-save", 0, sqlite_history_save, 0, 0, 0, "", NULL),
    BUILTIN("sqlite-history-list", 0, sqlite_history_list, 0, 0, 0, "zr", NULL),
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
    sqlite_handle = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    if (!sqlite_handle) {
	ERROR("could not open libsqlite3.so: %s", dlerror());
	return 1;
    }

    for (unsigned i = 0; i < countof(sqlite_function_names); i++) {
	const char *name = sqlite_function_names[i];
	void **dest = ((void **)&sqlite) + i;

	*dest = dlsym(sqlite_handle, name);
	if (!*dest) {
	    ERROR("could not find symbol %s: %s", name, dlerror());
	    goto err;
	}
    }

    return 0;

err:
    if (sqlite_handle) {
	dlclose(sqlite_handle);
	sqlite_handle = NULL;
    }
    memset(&sqlite, '\0', sizeof(sqlite));

    return 1;
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
    sqlite.finalize(g.insert);
    g.insert = NULL;

    int ret = sqlite.close(g.db);
    if (ret != SQLITE_OK) {
        ERROR("close db: %s", sqlite.errstr(ret));
    }
    g.db = NULL;

    g.session_id = LONG_MIN;
    g.verbose = false;

    if (sqlite_handle) {
	dlclose(sqlite_handle);
	sqlite_handle = NULL;
    }
    memset(&sqlite, '\0', sizeof(sqlite));

    return 0;
}


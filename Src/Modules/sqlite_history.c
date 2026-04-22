#include "sqlite_history.mdh"
#include "sqlite_history.pro"

#include <sqlite3.h>

static sqlite3 *db = NULL;

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
        return 1;
    }

    if (verbose) {
        printf("sqlite_history: opened db at %s\n", args[0]);
    }

    return 0;
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


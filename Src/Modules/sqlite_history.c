#include "sqlite_history.mdh"
#include "sqlite_history.pro"

#include <sqlite3.h>

/**/
static int
bin_sqlite_history(char *nam, char **args, Options ops, UNUSED(int func))
{
    unsigned char c;
    long i = 0;

    printf("Options: ");
    for (c = 32; ++c < 128;)
	if (OPT_ISSET(ops,c))
	    putchar(c);
    printf("\nArguments:");
    for (; *args; i++, args++) {
	putchar(' ');
	fputs(*args, stdout);
    }

    return 0;
}

static struct builtin bintab[] = {
    BUILTIN("sqlite_history", 0, bin_sqlite_history, 0, -1, 0, "flags", NULL),
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
    return 0;
}


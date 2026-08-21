/* /bin/ns — print a process's namespace, the way Plan 9's ns(1) does.
 *
 * This is the tool for the fault where nothing is corrupt: every file matches
 * its package and the machine still reads the wrong one, because something is
 * bound over the top of it.
 */
#include "gsys.h"
static char arg[64], path[64], body[2048];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    g_copy(path, "/proc/", sizeof path);
    g_cat(path, n > 0 ? v[0] : "self", sizeof path);
    g_cat(path, "/ns", sizeof path);
    if (g_slurp(path, body, sizeof body) < 0) { g_putln("ns: no such process"); g_exit(1); }
    if (!body[0]) { g_putln("(empty namespace: every path means itself)"); g_exit(0); }
    g_puts(body);
    g_exit(0);
}

/* /usr/bin/man — the manual, read off the disk like everything else. */
#include "gsys.h"
static char arg[128], path[192], body[8192], name[128];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 1) {
        g_putln("what manual page do you want?");
        g_putln("");
        for (int i = 0; i < 128; i++) {
            if (g_readdir("/usr/share/man", i, name) < 0) break;
            g_puts("  "); g_putln(name);
        }
        g_exit(1);
    }
    g_copy(path, "/usr/share/man/", sizeof path);
    g_cat(path, v[0], sizeof path);
    if (g_slurp(path, body, sizeof body) < 0) {
        g_puts("no manual entry for "); g_putln(v[0]);
        g_exit(1);
    }
    g_puts(body);
    g_exit(0);
}

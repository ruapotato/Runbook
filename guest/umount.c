/* /bin/umount — detach a filesystem. */
#include "gsys.h"
static char arg[256];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 1) { g_putln("usage: umount <mountpoint>"); g_exit(1); }
    if (sysc(SYS_umount, (i64)v[0], 0, 0) != 0) {
        g_puts("umount: "); g_puts(v[0]); g_putln(": not mounted"); g_exit(1);
    }
    g_exit(0);
}

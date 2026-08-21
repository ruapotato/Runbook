/* /bin/chmod — the fix for the commonest fault there is. */
#include "gsys.h"
static char arg[GARG_MAX];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 2) { g_putln("usage: chmod <octal> <path>"); g_exit(1); }
    g_argv_warn("chmod");
    unsigned m = 0;
    for (const char *s = v[0]; *s >= '0' && *s <= '7'; s++) m = m * 8 + (unsigned)(*s - '0');
    if (sysc(SYS_chmod, (i64)v[1], (i64)m, 0) != 0) {
        g_puts("chmod: "); g_puts(v[1]); g_putln(": no such file"); g_exit(1);
    }
    g_exit(0);
}

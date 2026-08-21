/* /bin/cp — copy a file, preserving its mode. */
#include "gsys.h"
static char arg[256], buf[65536];
void _start(void){
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 2) { g_putln("usage: cp <from> <to>"); g_exit(1); }
    NomStat st;
    if (g_stat(v[0], &st) != 0) { g_puts("cp: "); g_puts(v[0]); g_putln(": not found"); g_exit(1); }
    i64 n = g_slurp(v[0], buf, sizeof buf);
    if (n < 0) { g_puts("cp: "); g_puts(v[0]); g_putln(": cannot read"); g_exit(1); }
    int fd = g_open(v[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { g_puts("cp: "); g_puts(v[1]); g_putln(": cannot write"); g_exit(1); }
    sysc(SYS_write, fd, (i64)buf, n);
    g_close(fd);
    sysc(SYS_chmod, (i64)v[1], st.mode, 0);
    g_exit(0);
}

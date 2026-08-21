/* /bin/touch — create an empty file if it is not there. */
#include "gsys.h"
static char arg[GARG_MAX];
void _start(void){
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    if (n < 1) { g_putln("usage: touch <path>..."); g_exit(1); }
    g_argv_warn("touch");
    /* EVERY path. It created v[0] and dropped the rest without a word, so
     * `touch a b c` reported success having made one file, and `ls` a moment
     * later disagreed with the command that had just succeeded. */
    int bad = 0;
    for (int i = 0; i < n; i++) {
        int fd = g_open(v[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            g_puts("touch: "); g_puts(v[i]); g_putln(": cannot create");
            bad = 1;
            continue;
        }
        g_close(fd);
    }
    g_exit(bad);
}

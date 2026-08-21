/* /bin/cat — print a file. Refuses binaries, because a screen of ELF is not
 * evidence, it is noise. */
#include "gsys.h"
static char arg[GARG_MAX], buf[32768];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 1) { g_putln("usage: cat <file>"); g_exit(1); }
    g_argv_warn("cat");
    i64 n = g_slurp(v[0], buf, sizeof buf);
    if (n < 0) { g_puts("cat: "); g_puts(v[0]); g_putln(": cannot read"); g_exit(1); }
    int printable = 0, look = (int)(n < 200 ? n : 200);
    for (int i = 0; i < look; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 9 || c == 10 || (c >= 32 && c < 127)) printable++;
    }
    if (look && printable * 10 < look * 9) {
        g_puts("cat: "); g_puts(v[0]); g_puts(": binary file, ");
        g_putn(n); g_putln(" bytes");
        g_exit(1);
    }
    g_write(1, buf, (u64)n);
    if (n && buf[n-1] != '\n') g_puts("\n");
    g_exit(0);
}

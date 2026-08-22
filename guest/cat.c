/* /bin/cat — print files. Refuses binaries, because a screen of ELF is not
 * evidence, it is noise.
 *
 * IT TAKES MORE THAN ONE FILE, which it did not until the ship became a
 * directory of one-value files. `cat /dev/ship/hull /dev/ship/shields` is the
 * obvious thing to type the moment /dev/ship exists, and printing only the
 * first one silently is the worst possible answer: the player learns the
 * wrong number and nothing tells them.
 */
#include "gsys.h"
static char arg[GARG_MAX], buf[32768];

static int one(const char *path, int many)
{
    i64 n = g_slurp(path, buf, sizeof buf);
    if (n < 0) { g_puts("cat: "); g_puts(path); g_putln(": cannot read"); return 1; }
    int printable = 0, look = (int)(n < 200 ? n : 200);
    for (int i = 0; i < look; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 9 || c == 10 || (c >= 32 && c < 127)) printable++;
    }
    if (look && printable * 10 < look * 9) {
        g_puts("cat: "); g_puts(path); g_puts(": binary file, ");
        g_putn(n); g_putln(" bytes");
        return 1;
    }
    /* Only when there is more than one, the way cat has always done it: a
     * header over a single file would break every pipeline in every script
     * anybody has already written. */
    if (many) { g_puts("==> "); g_puts(path); g_putln(" <=="); }
    g_write(1, buf, (u64)n);
    if (n && buf[n-1] != '\n') g_puts("\n");
    return 0;
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    if (n < 1) { g_putln("usage: cat <file> [file ...]"); g_exit(1); }
    g_argv_warn("cat");
    int bad = 0;
    for (int i = 0; i < n; i++)
        if (one(v[i], n > 1)) bad = 1;
    g_exit(bad);
}

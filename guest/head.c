/* /bin/head — the first ten lines, or the first N.
 *
 * `head -20 f` and `head -n 20 f` both reported "-20: cannot read" and
 * "-n: cannot read", because every argument was treated as a filename. A tool
 * that names the flag you typed as though it were a missing file teaches the
 * player that the flag does not exist AND that the file is gone, and both of
 * those are wrong. Flags that work are implemented; flags that do not are
 * refused by name.
 *
 * It reads in chunks rather than slurping the file, so `head /var/log/messages`
 * on a 524 KB log costs one 4 KB read and stops.
 */
#include "gsys.h"

static char arg[GARG_MAX], buf[4096];
static char *v[GARGS];
static i64 want = 10;

/* The first `want` lines of an open fd. */
static void emit(int fd)
{
    i64 lines = 0;
    if (want <= 0) return;
    for (;;) {
        i64 n = g_read(fd, buf, sizeof buf);
        if (n <= 0) return;
        for (i64 i = 0; i < n; i++) {
            if (buf[i] != '\n') continue;
            if (++lines < want) continue;
            g_write(1, buf, (u64)(i + 1));
            return;
        }
        g_write(1, buf, (u64)n);
    }
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    int n = g_argv(arg, v);
    g_argv_warn("head");

    int nop = 0;
    for (int i = 0; i < n; i++) {
        if (v[i][0] != '-' || !v[i][1]) { v[nop++] = v[i]; continue; }
        /* -n N */
        if (g_streq(v[i], "-n")) {
            if (i + 1 >= n || !g_num(v[i+1], &want)) {
                g_putln("head: -n wants a number of lines");
                g_exit(2);
            }
            i++;
            continue;
        }
        /* -20, and -n20 while we are here */
        const char *d = v[i] + 1;
        if (*d == 'n') d++;
        if (g_num(d, &want)) continue;
        g_puts("head: "); g_puts(v[i]); g_putln(": not a flag this head has");
        g_putln("usage: head [-n N] [-N] [file ...]");
        g_exit(2);
    }

    if (nop == 0) { emit(0); g_exit(0); }   /* stdin: head at the end of a pipe */

    int rc = 0;
    for (int i = 0; i < nop; i++) {
        int fd = g_open(v[i], O_RDONLY);
        if (fd < 0) {
            g_puts("head: "); g_puts(v[i]); g_putln(": cannot read");
            rc = 1;
            continue;
        }
        /* The header is what tells you which file a line came from, and it
         * only appears when there is more than one file to confuse. */
        if (nop > 1) {
            if (i) g_putln("");
            g_puts("==> "); g_puts(v[i]); g_putln(" <==");
        }
        emit(fd);
        g_close(fd);
    }
    g_exit(rc);
}

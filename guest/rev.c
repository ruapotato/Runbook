/* /bin/rev — reverse each line.
 *
 * The smallest useful filter there is, and it earns its place twice. It is a
 * joke you can type at a prompt, and it is the shortest possible proof that
 * pipelines on this machine really carry bytes: `cat f | rev | rev` gives back
 * `cat f`, and if it does not, the pipe is what is broken and not your eyes.
 *
 *   rev [file ...]      with no file it reads stdin, which is the point
 *
 * The trailing newline is left where it was. Reversing it into the middle of
 * the next line would be funny exactly once.
 */
#include "gsys.h"

static char arg[GARG_MAX];
static char body[65536];
static char out[65536];

static void rev_buf(char *b, i64 len)
{
    i64 o = 0;
    i64 start = 0;
    for (i64 i = 0; i <= len; i++) {
        if (i == len || b[i] == '\n') {
            /* Strip a \r so a file that came off a DOS machine reverses to
             * something readable rather than to something with a carriage
             * return at the FRONT of every line. */
            i64 end = i;
            if (end > start && b[end - 1] == '\r') end--;
            for (i64 j = end - 1; j >= start; j--) out[o++] = b[j];
            if (i < len) out[o++] = '\n';
            start = i + 1;
        }
        if (o > (i64)sizeof out - 4) break;
    }
    g_write(1, out, (u64)o);
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    g_argv_warn("rev");

    if (n == 0) {
        i64 len = g_slurp_stdin(body, sizeof body);
        if (len > 0) rev_buf(body, len);
        g_exit(0);
    }

    int rc = 0;
    for (int i = 0; i < n; i++) {
        i64 len = g_slurp(v[i], body, sizeof body);
        if (len < 0) {
            g_puts("rev: ");
            g_puts(v[i]);
            g_putln(": cannot read");
            rc = 1;
            continue;
        }
        rev_buf(body, len);
    }
    g_exit(rc);
}

/* /bin/echo — print the arguments. A real program as well as a shell builtin,
 * because a builtin cannot be a stage in a pipeline.
 *
 * It used to print the raw argument string, which meant the QUOTES ended up in
 * the file: `echo "udev.* /dev/null" >> /etc/syslog.conf` wrote the line with
 * its own quote marks, and there was no way to write a line containing a space
 * without them. On a machine whose only editor is `echo >>` and `sed`, that is
 * the difference between being able to repair a config file and not.
 *
 *   -n   no trailing newline, so a file can be rebuilt byte-exactly
 */
#include "gsys.h"
static char arg[GARG_MAX];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    static char *v[GARGS];
    int n = g_argv(arg, v);          /* strips quotes and honours backslashes */
    g_argv_warn("echo");

    int i = 0, nl = 1;
    if (n > 0 && g_streq(v[0], "-n")) { nl = 0; i = 1; }

    for (; i < n; i++) {
        g_puts(v[i]);
        if (i + 1 < n) g_puts(" ");
    }
    if (nl) g_puts("\n");
    g_exit(0);
}

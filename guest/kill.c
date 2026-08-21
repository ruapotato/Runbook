/* /bin/kill — send a signal.
 *
 *   kill -HUP <pid>    tell it to re-read its configuration
 *   kill -TERM <pid>   ask it to stop
 *
 * Nothing is interrupted: there is no preemption on this machine, so the
 * signal sits pending until the process next looks. A well-behaved daemon
 * looks often. That is a smaller promise than a real kernel makes and it is
 * exactly enough for the thing signals are actually used for.
 */
#include "gsys.h"
static char arg[128];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    int sig = SIG_TERM, ai = 0;
    if (n >= 1 && v[0][0] == '-') {
        const char *s = v[0] + 1;
        if (g_streq(s, "HUP") || g_streq(s, "1"))       sig = SIG_HUP;
        else if (g_streq(s, "TERM") || g_streq(s, "15")) sig = SIG_TERM;
        else { g_putln("kill: only -HUP and -TERM are supported"); g_exit(1); }
        ai = 1;
    }
    if (n < ai + 1) { g_putln("usage: kill [-HUP|-TERM] <pid>"); g_exit(1); }
    int pid = 0;
    for (const char *s = v[ai]; *s >= '0' && *s <= '9'; s++) pid = pid * 10 + (*s - '0');
    if (g_kill(pid, sig) != 0) {
        g_puts("kill: no such running process: ");
        g_putln(v[ai]);
        g_exit(1);
    }
    g_exit(0);
}

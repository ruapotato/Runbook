/* /sbin/init — pid 1.
 *
 * Reads /etc/inittab and runs the last non-comment line, exactly as NomnixOS's
 * init2 does. It knows nothing else about booting: everything the machine
 * becomes is decided by files it reads at runtime, which is what makes this a
 * boot rather than a description of one.
 */
#include "gsys.h"

static char buf[8192];

void _start(void)
{
    /* ALSO TELINIT. On a real system /sbin/init is pid 1 AND the command a
     * user runs to change runlevel, and which one it is depends entirely on
     * whether it was given an argument. Without this, `init 0` spawned pid
     * 1's program again, it ignored the argument, and the whole boot sequence
     * ran a second time -- so every runlevel, including halt, looked like a
     * reboot. */
    {
        static char arg[64];
        g_getarg(arg, sizeof arg);
        char *v[GARGS];
        if (g_argv(arg, v) >= 1 && v[0][0] >= '0' && v[0][0] <= '9') {
            char r = v[0][0];
            if (r == '0') {
                g_putln("The system is going down for halt NOW!");
                sysc(SYS_reboot, 1, 0, 0);
                g_exit(0);
            }
            if (r == '6') {
                g_putln("The system is going down for reboot NOW!");
                sysc(SYS_reboot, 0, 0, 0);
                g_exit(0);
            }
            if (r == '1' || r == '3' || r == '5') {
                g_puts("init: switching to runlevel ");
                g_putln(v[0]);
                g_putln("  (this restarts the service set, so the machine boots again)");
                sysc(SYS_reboot, 0, 0, 0);
                g_exit(0);
            }
            g_puts("init: unknown runlevel: "); g_putln(v[0]);
            g_putln("  0 halt  1 single user  3 multi-user  5 graphical  6 reboot");
            g_exit(1);
        }
    }

    g_putln("init: pid 1 starting");

    if (g_slurp("/etc/inittab", buf, sizeof buf) < 0) {
        g_putln("init: /etc/inittab: cannot read");
        g_exit(1);
    }

    /* the last non-comment, non-blank line wins */
    static char cmd[256];
    cmd[0] = 0;
    char *p = buf;
    while (*p) {
        char *nl = p;
        while (*nl && *nl != '\n') nl++;
        char save = *nl;
        *nl = 0;
        char *line = g_trim(p);
        if (*line && *line != '#') g_copy(cmd, line, sizeof cmd);
        *nl = save;
        p = *nl ? nl + 1 : nl;
    }

    if (!cmd[0]) {
        g_putln("init: /etc/inittab: nothing to run");
        g_exit(1);
    }

    /* split "prog arg" */
    static char prog[192], arg[192];
    int i = 0;
    while (cmd[i] && cmd[i] != ' ') i++;
    char save = cmd[i];
    cmd[i] = 0;
    g_copy(prog, cmd, sizeof prog);
    cmd[i] = save;
    g_copy(arg, save ? g_trim(cmd + i) : "", sizeof arg);

    /* If the child failed it has already said why, in its own words, on this
     * same console. Printing "init: /bin/rc: failed" over the top would bury
     * the only evidence the player gets, so exit quietly with its status. */
    i64 rc = g_spawn(prog, arg);
    g_exit(rc == 0 ? 0 : 1);
}

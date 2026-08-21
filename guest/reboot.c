/* /sbin/reboot, /sbin/halt, /sbin/poweroff — the commands everyone types.
 *
 * `init 6` worked and `reboot` did not, which is backwards: nobody types
 * `init 6` first. All three are the same act with different endings, and they
 * go through the same restart the service processor uses, so a technician
 * watching the console sees exactly what a person at the keyboard would.
 */
#include "gsys.h"

void _start(void)
{
    static char arg[64];
    g_getarg(arg, sizeof arg);

    /* argv[0] is not available here, so the behaviour is chosen by the
     * argument: bare = reboot, -h/--halt = stop. */
    int halt = 0;
    char *v[GARGS];
    int n = g_argv(arg, v);
    for (int i = 0; i < n; i++)
        if (g_streq(v[i], "-h") || g_streq(v[i], "--halt") ||
            g_streq(v[i], "-p") || g_streq(v[i], "--poweroff")) halt = 1;

    g_putln(halt ? "The system is going down for halt NOW!"
                 : "The system is going down for reboot NOW!");
    if (sysc(SYS_reboot, halt, 0, 0) != 0) {
        g_putln("reboot: cannot restart this machine");
        g_exit(1);
    }
    g_exit(0);
}

/* /bin/chroot — make a mounted filesystem the root.
 *
 * After this, /etc is the customer's /etc and /bin/sh is the customer's shell.
 * That is the whole point: you stop looking at their disk from outside and
 * start running inside it, so the tools you use are theirs and the paths in
 * their config mean what they mean to them.
 */
#include "gsys.h"
static char arg[256];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    if (g_argv(arg, v) < 1) { g_putln("usage: chroot <dir>"); g_exit(1); }
    /* SAY WHICH OF THE FOUR THINGS WENT WRONG. "not a directory (is anything
     * mounted there?)" was printed for every failure, including the two where
     * the kernel knew the exact answer -- and a question mark in an error
     * message is the tool admitting it did not look. */
    i64 rc = sysc(SYS_chroot, (i64)v[0], 0, 0);
    if (rc == -3) {
        g_puts("chroot: "); g_puts(v[0]);
        g_putln(": nothing is mounted there. Mount the disk first:");
        g_puts("      mount /dev/sda1 "); g_putln(v[0]);
        g_exit(1);
    }
    if (rc == -4) {
        g_puts("chroot: "); g_puts(v[0]);
        g_putln(": mounted, but there is no /bin/sh in it -- that is not a");
        g_putln("  root filesystem. `ls` it and check what you mounted.");
        g_exit(1);
    }
    if (rc == -2) {
        g_puts("chroot: "); g_puts(v[0]);
        g_putln(": its /bin/sh cannot run -- the libraries it needs are");
        g_putln("  missing or the wrong version, so nothing inside would work.");
        g_puts("  Repair it from out here instead: pkg --root ");
        g_puts(v[0]); g_putln(" verify");
        g_exit(1);
    }
    if (rc != 0) {
        g_puts("chroot: "); g_puts(v[0]);
        g_putln(": not a directory");
        g_exit(1);
    }
    g_puts("chroot: root is now "); g_putln(v[0]);
    g_exit(0);
}

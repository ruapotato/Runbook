/* /bin/mount — attach a filesystem, or show what is attached.
 *
 *   mount                     print the table
 *   mount /dev/sda1 /mnt      mount a block device
 *   mount /dev /mnt/dev       bind an existing tree (what you do before chroot)
 */
#include "gsys.h"
static char arg[256], table[2048];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    if (n == 0) {
        i64 got = sysc(SYS_mounts, (i64)table, sizeof table, 0);
        /* "nothing mounted" was never true: the root filesystem is mounted
         * or nothing here could run. It was simply left out of the table. */
        if (got <= 0) g_putln("mount: the kernel would not answer");
        else g_write(1, table, (u64)got);
        g_exit(0);
    }
    if (n < 2) { g_putln("usage: mount [<device|dir> <mountpoint>]"); g_exit(1); }

    /* Anything that is not a block device is a bind of an existing tree.
     * mount(8) would want -o bind; here the distinction is unambiguous
     * because a device name always starts /dev/. */
    int flags = 0;
    if (!(v[0][0] == '/' && v[0][1] == 'd' && v[0][2] == 'e' && v[0][3] == 'v'
          && v[0][4] == '/')) flags = MNT_BIND;
    else {
        NomStat st;
        if (g_stat(v[0], &st) != 0) flags = MNT_BIND;
    }

    if (sysc(SYS_mount, (i64)v[0], (i64)v[1], flags) != 0) {
        g_puts("mount: cannot mount ");
        g_puts(v[0]);
        g_puts(" on ");
        g_puts(v[1]);
        g_putln(" -- does the mountpoint exist, and is it free?");
        g_exit(1);
    }
    g_exit(0);
}

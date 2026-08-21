/* /sbin/fsck — check and repair a filesystem.
 *
 * Run against an UNMOUNTED device. Metadata it can rebuild; the contents of
 * whatever was being written when the power went it cannot, so a dirty
 * filesystem is usually two repairs: fsck, then whatever pkg verify finds.
 */
#include "gsys.h"
static char arg[128], report[2048];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    const char *dev = n > 0 ? v[0] : "/dev/sda1";

    i64 rc = sysc(SYS_fsck, (i64)dev, (i64)report, sizeof report);
    g_puts(report);
    if (rc < 0) g_exit(1);
    g_exit(0);
}

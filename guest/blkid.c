/* /sbin/blkid — what uuid does this disk actually carry?
 *
 * Several faults hinge on a uuid that is well-formed and simply not this
 * disk's: zbl.cfg names one, /etc/fstab names another, and the initrd waits
 * for a device that will never appear. Without this there is no way to check
 * either of them against reality, only against each other -- and two configs
 * agreeing with each other and not with the disk is exactly the fault.
 */
#include "gsys.h"
static char uuid[64];
void _start(void)
{
    if (g_rootuuid(uuid, sizeof uuid) <= 0) {
        g_putln("blkid: cannot read the disk");
        g_exit(1);
    }
    /* The type is asked for, not asserted. blkid that prints a constant is
     * an oracle agreeing with itself; this one probes the device, so when
     * fstab claims a type the device does not have, blkid and mount tell the
     * same story and the file is the odd one out. */
    static char t1[32], t2[32];
    i64 n1 = sysc(SYS_fstype, (i64)"/dev/sda1", (i64)t1, sizeof t1 - 1);
    i64 n2 = sysc(SYS_fstype, (i64)"/dev/sr0",  (i64)t2, sizeof t2 - 1);
    t1[n1 > 0 ? n1 : 0] = 0;
    t2[n2 > 0 ? n2 : 0] = 0;
    g_puts("/dev/sda1: UUID=\"");
    g_puts(uuid);
    g_puts("\" TYPE=\"");
    g_puts(t1);
    g_putln("\"");
    /* AN EMPTY DRIVE HAS NO FILESYSTEM ON IT, and TYPE="" reads as a probe
     * that failed rather than a drive that is empty. The removable device is
     * the one thing here whose presence changes during a ticket, so it is the
     * one that has to say which of the two it is. */
    if (n2 <= 0) {
        g_putln("/dev/sr0:  no medium (the drive is empty)");
    } else {
        g_puts("/dev/sr0:  TYPE=\"");
        g_puts(t2);
        g_putln("\"");
    }
    g_exit(0);
}

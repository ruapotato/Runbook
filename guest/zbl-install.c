/* /usr/sbin/zbl-install — write the boot sector.
 *
 * The boot sector is not a file. No package owns it, `pkg verify` cannot see
 * it, and no amount of reinstalling will bring it back. When the firmware
 * says "no bootable device" and every file on the disk is perfect, this is
 * the tool.
 */
#include "gsys.h"
static char arg[128];
void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    const char *dev = n > 0 ? v[0] : "/dev/sda";

    NomStat st;
    if (g_stat("/boot/zbl/zbl.cfg", &st) != 0) {
        g_putln("zbl-install: /boot/zbl/zbl.cfg is missing -- write a");
        g_putln("             configuration before installing the loader");
        g_exit(1);
    }
    g_bootsec(1);
    g_puts("zbl-install: boot sector written to ");
    g_putln(dev);
    g_exit(0);
}

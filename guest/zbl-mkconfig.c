/* /usr/sbin/zbl-mkconfig — regenerate the loader configuration.
 *
 * Reads the uuid the disk ACTUALLY has and writes a config that matches
 * reality. That is the difference between this and reinstalling the package:
 * the package ships a config for the machine it was built for, this one is
 * generated from the machine in front of you.
 */
#include "gsys.h"
static char uuid[64], out[512];
void _start(void)
{
    if (g_rootuuid(uuid, sizeof uuid) <= 0) {
        g_putln("zbl-mkconfig: cannot determine the root filesystem uuid");
        g_exit(1);
    }
    NomStat st;
    const char *kern = "/boot/vmnomuz", *ird = "/boot/initrd";
    if (g_stat(kern, &st) != 0) {
        g_putln("zbl-mkconfig: /boot/vmnomuz is not there -- fix the kernel");
        g_putln("              package first, or the config will point at");
        g_putln("              something that does not exist");
        g_exit(1);
    }
    if (g_stat(ird, &st) != 0) {
        g_putln("zbl-mkconfig: /boot/initrd is not there");
        g_exit(1);
    }

    g_copy(out, "default 0\ntimeout 5\n\nentry \"NomnixOS 11.4\"\n", sizeof out);
    g_cat(out, "  kernel /boot/vmnomuz\n", sizeof out);
    g_cat(out, "  initrd /boot/initrd\n", sizeof out);
    g_cat(out, "  root UUID=", sizeof out);
    g_cat(out, uuid, sizeof out);
    g_cat(out, "\n", sizeof out);

    int fd = g_open("/boot/zbl/zbl.cfg", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { g_putln("zbl-mkconfig: cannot write /boot/zbl/zbl.cfg"); g_exit(1); }
    sysc(SYS_write, fd, (i64)out, (i64)g_strlen(out));
    g_close(fd);
    g_puts("zbl-mkconfig: wrote /boot/zbl/zbl.cfg for UUID=");
    g_putln(uuid);
    g_exit(0);
}

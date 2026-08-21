/* /usr/bin/mkinitrd — rebuild the initial ramdisk from the installed modules.
 *
 * The initrd is BUILT, not shipped: it is assembled out of whatever is in
 * /lib/modules. So if a module is missing from the disk, rebuilding cannot
 * invent it -- you have to fix the kernel package first and then rebuild.
 * That is two different repairs in sequence, and it is how it really works.
 */
#include "gsys.h"
static char out[1024], name[128], path[192];

static const char *NEEDED[] = { "virtio_blk", "ext4", "dm_mod", 0 };

void _start(void)
{
    static const char *MODDIR = "/lib/modules/6.4.11";
    int missing = 0;
    for (int i = 0; NEEDED[i]; i++) {
        g_copy(path, MODDIR, sizeof path);
        g_cat(path, "/", sizeof path);
        g_cat(path, NEEDED[i], sizeof path);
        g_cat(path, ".ko", sizeof path);
        NomStat st;
        if (g_stat(path, &st) != 0) {
            g_puts("mkinitrd: missing module: ");
            g_putln(path);
            missing++;
        }
    }
    if (missing) {
        g_putln("mkinitrd: cannot build an initrd without the root device and");
        g_putln("          filesystem drivers. `pkg reinstall kernel-default`");
        g_putln("          then run this again.");
        g_exit(1);
    }

    g_copy(out, "\x7fINITRD 6.4.11\n", sizeof out);
    for (int i = 0; i < 128; i++) {
        if (g_readdir(MODDIR, i, name) < 0) break;
        if (!g_endswith(name, ".ko")) continue;
        g_cat(out, "module ", sizeof out);
        u64 l = g_strlen(name);
        name[l - 3] = 0;                 /* drop .ko */
        g_cat(out, name, sizeof out);
        g_cat(out, "\n", sizeof out);
    }

    int fd = g_open("/boot/initrd-6.4.11", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { g_putln("mkinitrd: cannot write /boot/initrd-6.4.11"); g_exit(1); }
    sysc(SYS_write, fd, (i64)out, (i64)g_strlen(out));
    g_close(fd);
    g_putln("mkinitrd: rebuilt /boot/initrd-6.4.11 from /lib/modules/6.4.11");
    g_exit(0);
}

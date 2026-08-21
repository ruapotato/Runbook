/* /bin/dmesg — what the machine said while it was booting.
 *
 * This is meant to be the FIRST thing you run, not the last. The boot log
 * tells you which layer failed -- loader, initrd, root filesystem, libraries,
 * services, login -- and that is the question worth answering before you go
 * anywhere near the package database. `pkg verify` is precise and it is
 * expensive and it is best asked about a package you already suspect.
 *
 *   dmesg            this boot
 *   dmesg -1         the PREVIOUS boot -- usually the interesting one, because
 *                    the customer rebooted before they rang you
 *   dmesg -f <text>  only lines containing <text>
 *
 * On the rescue medium the customer's log is under the mount point:
 *   dmesg -r /mnt          (their current boot)
 *   dmesg -r /mnt -1       (the boot that failed before they called)
 */
#include "gsys.h"

static char buf[32768];
static char path[256];

static void show(const char *p, i64 n, const char *filter)
{
    if (!filter) { g_write(1, p, (u64)n); return; }
    u64 fl = g_strlen(filter);
    i64 i = 0;
    while (i < n) {
        i64 e = i;
        while (e < n && p[e] != '\n') e++;
        int hit = 0;
        for (i64 q = i; q + (i64)fl <= e && !hit; q++) {
            u64 k = 0;
            while (k < fl && p[q + k] == filter[k]) k++;
            if (k == fl) hit = 1;
        }
        if (hit) g_write(1, p + i, (u64)(e - i + (e < n ? 1 : 0)));
        i = e < n ? e + 1 : n;
    }
}

void _start(void)
{
    static char arg[256];
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    const char *root = "";
    const char *filter = 0;
    int prev = 0;
    for (int i = 0; i < n; i++) {
        if (g_streq(v[i], "-1") || g_streq(v[i], "--prev")) prev = 1;
        else if (g_streq(v[i], "-r") && i + 1 < n) root = v[++i];
        else if (g_streq(v[i], "-f") && i + 1 < n) filter = v[++i];
        else {
            g_puts("dmesg: unknown option: "); g_putln(v[i]);
            g_putln("usage: dmesg [-r <root>] [-1] [-f <text>]");
            g_exit(1);
        }
    }

    g_copy(path, root, sizeof path);
    g_cat(path, prev ? "/var/log/boot.log.1" : "/var/log/boot.log", sizeof path);

    i64 got = g_slurp(path, buf, sizeof buf);
    if (got < 0) {
        g_puts("dmesg: "); g_puts(path); g_putln(": no boot log there.");
        if (prev)
            g_putln("  There is only one previous boot kept, and this machine\n"
                    "  may not have booted twice since the log was last lost.");
        else
            g_putln("  A machine writes this as it boots, so an empty one is\n"
                    "  itself a finding: either it never got far enough to have\n"
                    "  a writable root, or /var/log is not there to write to.");
        g_exit(1);
    }
    show(buf, got, filter);
    g_exit(0);
}

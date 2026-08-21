/* /sbin/mountall — bring up everything in /etc/fstab.
 *
 * Read by rc.boot, before anything that needs a filesystem to be there. An
 * entry that names a device this machine does not have stops the boot, with
 * the line number, because that is what you need to know and it is the one
 * thing the machine can tell you for certain.
 *
 * The `noauto` option is honoured, so an entry can legitimately be present
 * and not mounted -- which matters, because "it is in fstab" and "it is
 * mounted" are different claims and confusing them is a classic.
 */
#include "gsys.h"

static char fstab[8192], line[256];

static int has_opt(const char *opts, const char *want)
{
    u64 wl = g_strlen(want);
    for (u64 i = 0; opts[i]; i++) {
        u64 k = 0;
        while (k < wl && opts[i + k] == want[k]) k++;
        if (k == wl && (opts[i + k] == 0 || opts[i + k] == ','))
            if (i == 0 || opts[i - 1] == ',') return 1;
    }
    return 0;
}

void _start(void)
{
    if (g_slurp("/etc/fstab", fstab, sizeof fstab) < 0) {
        g_putln("mountall: /etc/fstab: cannot read");
        g_exit(1);
    }

    int lineno = 0, mounted = 0;
    char *p = fstab;
    while (*p) {
        char *nl = p; while (*nl && *nl != '\n') nl++;
        char save = *nl; *nl = 0;
        g_copy(line, p, sizeof line);
        *nl = save; p = *nl ? nl + 1 : nl;
        lineno++;

        char *t = g_trim(line);
        if (!*t || *t == '#') continue;

        char *v[GARGS];
        int n = g_argv(t, v);
        if (n < 3) {
            g_puts("mountall: /etc/fstab:");
            g_putn(lineno);
            g_puts(": needs at least a device, a mount point and a type: ");
            g_putln(t);
            g_exit(1);
        }
        const char *dev = v[0], *at = v[1], *type = v[2];
        const char *opts = n > 3 ? v[3] : "defaults";

        if (has_opt(opts, "noauto")) continue;

        if (at[0] != '/') {
            g_puts("mountall: /etc/fstab:");
            g_putn(lineno);
            g_puts(": mount point is not an absolute path: ");
            g_putln(at);
            g_exit(1);
        }

        /* What fstab CLAIMS versus what the device actually is. mount(8)
         * probes rather than trusting the file, so a type that does not match
         * fails with a specific and very recognisable complaint instead of a
         * mysterious one. Virtual filesystems have no device to probe. */
        if (dev[0] == '/' || (dev[0] == 'U' && dev[1] == 'U')) {
            static char real[32];
            i64 tl = sysc(SYS_fstype, (i64)dev, (i64)real, sizeof real - 1);
            if (tl > 0) {
                real[tl] = 0;
                if (!g_streq(real, type)) {
                    g_puts("mountall: ");
                    g_puts(dev);
                    g_puts(": wrong fs type: /etc/fstab says ");
                    g_puts(type);
                    g_puts(", the device is ");
                    g_putln(real);
                    g_exit(1);
                }
            } else if (dev[0] == 'U' && !has_opt(opts, "nofail")) {
                /* A UUID NAMES A DISK, AND EITHER IT IS HERE OR IT IS NOT.
                 *
                 * A device path that is absent fails later, at the mount, with
                 * a message about the device. A uuid cannot even be looked up:
                 * nothing on this machine carries it. That is a different and
                 * commoner mistake -- a disk was replaced, or the entry was
                 * copied from another machine's fstab -- and it deserves to be
                 * said in those words, because the fix is to find out what
                 * this disk's uuid really is (`blkid`) rather than to hunt for
                 * a device node. */
                g_puts("mountall: /etc/fstab:");
                g_putn(lineno);
                g_puts(": ");
                g_puts(dev);
                g_putln(": no device on this machine has that uuid");
                g_puts("          `blkid` says what /dev/sda1 actually is.\n");
                g_exit(1);
            }
        }

        /* The root is already mounted by the initrd, read-only, exactly as a
         * real machine does it. What happens here is the REMOUNT: fstab says
         * how the running system wants its root, and this is where that word
         * takes effect. An fstab that says ro means the remount to read-write
         * never happens and the machine comes up unable to write to its own
         * disk -- nothing corrupt, every hash matching, and nothing working.
         *
         * Announcing it matters. A read-only root fails as a cascade of
         * unrelated-looking errors from whichever daemon writes first, and
         * without this line there is nothing tying them together. */
        if (g_streq(at, "/")) {
            if (has_opt(opts, "ro")) {
                sysc(SYS_mount, (i64)dev, (i64)"/", MNT_RO);
                g_putln("mountall: / is mounted read-only (fstab says ro)");
            } else {
                sysc(SYS_mount, (i64)dev, (i64)"/", 0);
            }
            continue;
        }

        NomStat st;
        if (g_stat(at, &st) != 0) {
            g_puts("mountall: ");
            g_puts(at);
            g_putln(": mount point does not exist");
            if (has_opt(opts, "nofail")) continue;
            g_exit(1);
        }
        if (st.kind != NOM_KIND_DIR) {
            g_puts("mountall: ");
            g_puts(at);
            g_putln(": mount point is not a directory");
            g_exit(1);
        }

        if (sysc(SYS_mount, (i64)dev, (i64)at, 0) != 0) {
            g_puts("mountall: /etc/fstab:");
            g_putn(lineno);
            g_puts(": cannot mount ");
            g_puts(dev);
            g_puts(" on ");
            g_puts(at);
            g_putln("");
            /* `nofail` IS THE WHOLE DIFFERENCE between an entry for a disk
             * that is not here and a boot that stops. Every administrator who
             * has added a second disk knows the option, and until now this
             * program did not, so a perfectly ordinary fstab line for a drive
             * that is out of the machine took the boot down whatever it said.
             * With it, the same missing device is a line on the console and
             * nothing more -- which is what makes an fstab entry for an absent
             * disk sometimes a fault and sometimes just housekeeping. */
            if (has_opt(opts, "nofail")) {
                g_puts("          (nofail: carrying on without it)\n");
                continue;
            }
            g_puts("          the device is not there. Check the entry, or "
                   "add noauto if\n          it is not supposed to be.\n");
            g_exit(1);
        }
        g_puts("mountall: mounted ");
        g_puts(dev);
        g_puts(" on ");
        g_putln(at);
        mounted++;
    }
    g_exit(0);
}

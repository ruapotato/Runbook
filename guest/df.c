/* /bin/df — how much room is left, and what is mounted where.
 *
 * Space first, because that is what df is for and because a full disk is a
 * fault no amount of verifying will find: every file is exactly what it
 * should be, there is simply nowhere to put the next one.
 */
#include "gsys.h"
static char t[2048];
static char arg[128];
/* NOTHING TO MEASURE IS AN ANSWER; A NUMBER ABOUT AN UNMOUNTED FILESYSTEM
 * IS NOT.
 *
 * After `umount /mnt` on the rescue medium df printed a full
 * "/dev/sda1 1035K 523K 511K 50%" row and then, under its own second header a
 * few lines later, "(nothing mounted)". One command, two halves, flatly
 * contradicting each other about the same filesystem in one screenful.
 *
 * The kernel now refuses the question when the disk is not mounted anywhere,
 * and refusing it is the whole of the fix -- the number was never available,
 * it was simply being printed anyway. */
static void unreachable(const char *what)
{
    g_puts("(the customer's disk is not mounted -- no ");
    g_puts(what);
    g_putln(" to report)");
    g_putln("");
    g_putln("/dev/sda1 is a device until something mounts it, and this");
    g_putln("machine is running a live image with no space of its own.");
    g_putln("Mount it and df will measure it:");
    g_putln("      mount /dev/sda1 /mnt");
}

void _start(void)
{
    /* -i: INODES, not bytes. A filesystem runs out of the two independently,
     * and the second one is the confusing one -- plenty of space, every write
     * refused, nothing corrupt. Nothing but this will tell you. */
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n0 = g_argv(arg, v);
    int inodes = 0;
    for (int i = 0; i < n0; i++) if (g_streq(v[i], "-i")) inodes = 1;

    if (inodes) {
        i64 iu = sysc(SYS_dfused, 2, 0, 0);
        i64 ic = sysc(SYS_dfused, 3, 0, 0);
        if (ic < 0 || iu < 0) {
            g_putln("FILESYSTEM      INODES     IUSED     IFREE  IUSE%");
            unreachable("inodes");
            g_exit(1);
        }
        g_putln("FILESYSTEM      INODES     IUSED     IFREE  IUSE%");
        g_puts("/dev/sda1     ");
        /* A filesystem cannot have more inodes in use than it has, and free
         * cannot be negative. It printed 672/674/-1, which reads as a broken
         * tool rather than a full one. */
        if (iu > ic) iu = ic;
        g_putn(ic); g_puts("      ");
        g_putn(iu); g_puts("      ");
        g_putn(ic - iu); g_puts("      ");
        g_putn(ic ? (iu * 100 / ic) : 0);
        g_putln("%");
        if (ic && iu >= ic)
            g_putln("\nno free inodes: nothing can create a file, however much\n"
                    "space `df` says is left. Something has made a very large\n"
                    "number of files somewhere.");
        g_exit(0);
    }

    i64 used = sysc(SYS_dfused, 0, 0, 0);
    i64 cap  = sysc(SYS_dfused, 1, 0, 0);
    if (cap < 0 || used < 0) {
        g_putln("FILESYSTEM       SIZE     USED    AVAIL  USE%");
        unreachable("space");
        g_putln("");
    } else if (cap > 0) {
        g_putln("FILESYSTEM       SIZE     USED    AVAIL  USE%");
        g_puts("/dev/sda1     ");
        g_putn(cap / 1024);  g_puts("K   ");
        g_putn(used / 1024); g_puts("K   ");
        g_putn((cap - used) / 1024); g_puts("K   ");
        g_putn(cap ? (used * 100 / cap) : 0);
        g_putln("%");
        g_putln("");
    }
    i64 n = sysc(SYS_mounts, (i64)t, sizeof t, 0);
    g_putln("FILESYSTEM        MOUNTED ON");
    /* There is always at least one line: something is mounted on /, or this
     * process could not be reading a file. The table used to leave the root
     * filesystem out and print "(nothing mounted)" underneath a top half that
     * had just measured /dev/sda1 -- one command, two halves, disagreeing
     * about whether the disk was mounted. Silence here now means the kernel
     * refused the question, which is a different thing and says so. */
    if (n > 0) g_write(1, t, (u64)n);
    else       g_putln("(the kernel would not answer -- no mount table)");
    g_exit(0);
}

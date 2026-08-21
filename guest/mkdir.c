/* /bin/mkdir — make a directory.
 *
 * docs/protocol.md described it and the machine did not have it, which is the
 * worst of both worlds: the player reads that it exists, types it, and is told
 * it is not a command. There was no way at all to create a directory on this
 * system -- open(O_CREAT) deliberately refuses to invent the ones above a
 * file -- so a missing /var/log could be diagnosed and not repaired.
 *
 * -p makes the parents too, and makes an existing directory a success rather
 * than an error, which is the only reason anyone types it.
 */
#include "gsys.h"

static char arg[GARG_MAX];
static char *v[GARGS];

void _start(void)
{
    g_getarg(arg, sizeof arg);
    int n = g_argv(arg, v);
    g_argv_warn("mkdir");

    int parents = 0, nop = 0;
    for (int i = 0; i < n; i++) {
        if (v[i][0] != '-' || !v[i][1]) { v[nop++] = v[i]; continue; }
        if (g_streq(v[i], "-p")) { parents = 1; continue; }
        g_puts("mkdir: "); g_puts(v[i]); g_putln(": not a flag this mkdir has");
        g_putln("usage: mkdir [-p] <dir> ...");
        g_exit(2);
    }
    if (nop < 1) { g_putln("usage: mkdir [-p] <dir> ..."); g_exit(2); }

    int rc = 0;
    for (int i = 0; i < nop; i++) {
        if (g_mkdir(v[i], parents) == 0) continue;
        rc = 1;
        /* Which of the several reasons it was. A bare "failed" sends the
         * player looking in the wrong place, and the reasons here are all
         * faults this machine really has: a missing parent, a read-only
         * root, a directory with no write bit, no inodes left. */
        g_puts("mkdir: "); g_puts(v[i]); g_puts(": ");
        NomStat st;
        if (g_stat(v[i], &st) == 0) { g_putln("already exists"); continue; }
        g_putln("cannot create -- check the directory above it exists and is");
        g_putln("  writable, and that `df` and `df -i` have anything left");
    }
    g_exit(rc);
}

/* /bin/rm — remove files, and with -r whole directories.
 *
 * -r exists because of a machine a playtester could not repair: four hundred
 * stale files in /tmp, no glob, no -r, no find. Globbing now works, but a
 * glob that expands to four hundred paths does not fit in an argument list,
 * so the honest fix for "a directory full of rubbish" is the flag everyone
 * already reaches for.
 */
#include "gsys.h"

static char arg[GARG_MAX];

/* Depth first, because a directory cannot go until it is empty. */
static int rm_tree(const char *path, int depth)
{
    if (depth > 12) return 1;
    int bad = 0;
    /* ON THE STACK, BECAUSE THIS FUNCTION CALLS ITSELF.
     *
     * `static` here is the bug this project has been bitten by twice: the
     * recursive call overwrites the caller's name and path, so the outer
     * level comes back out of a subdirectory reading whatever the inner
     * level left behind. It survived only because a subdirectory's entries
     * happen to be listed before the parent finishes with them. Twelve
     * frames of half a kilobyte in a program with four megabytes. */
    char nm[160];
    /* Re-read index 0 each time: the listing shifts as entries go. */
    for (int guard = 0; guard < 4096; guard++) {
        if (g_readdir(path, 0, nm) < 0) break;
        char child[320];
        g_copy(child, path, sizeof child);
        if (!g_streq(path, "/")) g_cat(child, "/", sizeof child);
        g_cat(child, nm, sizeof child);
        NomStat st;
        if (g_stat(child, &st) == 0 && st.kind == NOM_KIND_DIR)
            bad |= rm_tree(child, depth + 1);
        else if (sysc(SYS_unlink, (i64)child, 0, 0) != 0)
            bad = 1;
    }
    if (sysc(SYS_unlink, (i64)path, 0, 0) != 0) bad = 1;
    return bad;
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    g_argv_warn("rm");
    int rec = 0, i = 0;
    if (n > 0 && (g_streq(v[0], "-r") || g_streq(v[0], "-rf") ||
                  g_streq(v[0], "-fr"))) { rec = 1; i = 1; }
    if (n <= i) { g_putln("usage: rm [-r] <path>..."); g_exit(1); }

    int bad = 0;
    for (; i < n; i++) {
        NomStat st;
        if (rec && g_stat(v[i], &st) == 0 && st.kind == NOM_KIND_DIR) {
            if (rm_tree(v[i], 0)) {
                g_puts("rm: "); g_puts(v[i]); g_putln(": could not remove everything");
                bad = 1;
            }
            continue;
        }
        if (sysc(SYS_unlink, (i64)v[i], 0, 0) != 0) {
            g_puts("rm: "); g_puts(v[i]); g_putln(": cannot remove"); bad = 1;
        }
    }
    g_exit(bad);
}

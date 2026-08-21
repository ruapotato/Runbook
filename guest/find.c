/* /usr/bin/find — walk a tree and print what is in it.
 *
 * It exists because the model kept reaching for it and a playtester kept
 * wanting it. Both were right: "where did that file go" is a question every
 * administrator asks, and answering it with `ls` in a loop is not an answer.
 *
 *   find <dir>                  everything under <dir>
 *   find <dir> -name <pattern>  only names matching (* and ? work)
 *   find <dir> -type f|d        only files, or only directories
 *
 * Depth first, so the output reads like a tree rather than a queue.
 */
#include "gsys.h"

static char arg[GARG_MAX];
static const char *pat = 0;
static int want_kind = 0;          /* 0 any, 1 file, 2 dir */
static int hits = 0;

static int match(const char *p, const char *nm)
{
    while (*p && *nm) {
        if (*p == '*') {
            p++;
            if (!*p) return 1;
            for (const char *q = nm; *q; q++) if (match(p, q)) return 1;
            return 0;
        }
        if (*p != '?' && *p != *nm) return 0;
        p++; nm++;
    }
    while (*p == '*') p++;
    return !*p && !*nm;
}

/* ONE PATH BUFFER, GROWN AND TRUNCATED -- NOT A STATIC PER FRAME.
 *
 * This walked into a subdirectory and came back out having listed exactly
 * one of its entries. `find /usr/share/man` printed one line where `ls -l`
 * printed eight, which made find useless for the question it exists to
 * answer, and worse than useless as evidence: a player who ran it would
 * conclude a directory was nearly empty when it was not.
 *
 * The cause was `static char child[320]` inside a recursive function. The
 * recursive call was handed `child` as its `dir`, then immediately appended
 * its own entry name to that same buffer -- so `dir` changed under the
 * caller's feet, and the next `g_readdir(dir, i, nm)` read some other
 * directory entirely. `static` inside a recursive function is one buffer
 * shared by every depth, which is precisely what recursion must not have.
 *
 * A stack buffer per frame would fix it and cost 480 bytes a level on a
 * freestanding stack. One shared buffer, appended to on the way down and
 * truncated on the way back up, costs nothing and cannot alias: `len` is
 * the caller's own, and restoring it is the whole of the bookkeeping.
 */
static char path[512];

static void walk(int len, int depth)
{
    if (depth > 12 || hits > 4000) return;
    char nm[160];
    for (int i = 0; i < 4096; i++) {
        path[len] = 0;
        if (g_readdir(path, i, nm) < 0) break;

        int end = len;
        if (!(len == 1 && path[0] == '/')) path[end++] = '/';
        for (int k = 0; nm[k] && end < (int)sizeof path - 1; k++)
            path[end++] = nm[k];
        path[end] = 0;

        NomStat st;
        int isdir = (g_stat(path, &st) == 0 && st.kind == NOM_KIND_DIR);
        int kind_ok = (want_kind == 0) || (want_kind == 1 && !isdir)
                                       || (want_kind == 2 && isdir);
        if (kind_ok && (!pat || match(pat, nm))) {
            g_putln(path);
            hits++;
        }
        if (isdir) walk(end, depth + 1);
    }
    path[len] = 0;
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    if (n < 1) { g_putln("usage: find <dir> [-name <pattern>] [-type f|d]"); g_exit(1); }

    const char *root = v[0];
    for (int i = 1; i + 1 < n; i++) {
        if (g_streq(v[i], "-name")) pat = v[++i];
        else if (g_streq(v[i], "-type")) {
            i++;
            want_kind = v[i][0] == 'd' ? 2 : 1;
        }
    }
    NomStat st;
    if (g_stat(root, &st) != 0) {
        g_puts("find: "); g_puts(root); g_putln(": no such directory");
        g_exit(1);
    }
    if (st.kind != NOM_KIND_DIR) { g_putln(root); g_exit(0); }
    g_copy(path, root, sizeof path);
    int rlen = 0;
    while (path[rlen]) rlen++;
    /* A trailing slash on the root would double up on every child. */
    while (rlen > 1 && path[rlen - 1] == '/') path[--rlen] = 0;
    walk(rlen, 0);
    if (!hits) g_putln("(nothing matched)");
    g_exit(0);
}

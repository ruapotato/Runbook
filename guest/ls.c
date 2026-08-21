/* /bin/ls — list a directory, with modes, so permission damage is visible.
 *
 * MANY OPERANDS. This took the FIRST argument and threw the rest away, which
 * was invisible until globbing started working. A glob of the nine .conf
 * files in /etc expands to nine paths and printed ONE; a glob of everything
 * in /etc beginning with n expanded to six and listed the CONTENTS of
 * /etc/nomde, silently dropping the other five. Both look like
 * answers. Real ls takes every operand: the plain files first, then each
 * directory, with a `name:` header when there is more than one thing to show.
 *
 * A FILE. `ls -l somefile` printed the bare path with no mode and no size --
 * which is exactly the row you reach for right after `stat`, and the one thing
 * it would not give you. A file operand now prints the same long-form row it
 * would print inside its directory.
 */
#include "gsys.h"

static char arg[GARG_MAX], name[256], full[512];
static char *v[GARGS];
static int show_all;      /* -a: include names beginning with a dot */
static int dir_itself;    /* -d: the directory, not what is in it   */

/* One long-form row. `path` is what we stat; `label` is what we print, so a
 * directory entry shows its bare name and an operand shows the path you
 * typed. */
static void row(const char *path, const char *label)
{
    static char tgt[192];
    i64 tl = g_readlink(path, tgt, sizeof tgt);
    NomStat st;
    if (tl > 0) {
        /* A LINK, and readlink is the only thing that knows it. stat follows
         * the link, so its kind is the TARGET's kind and a healthy symlink
         * rendered as a plain file showing the target's size -- while a
         * dangling one rendered as `l`. The type column therefore told you `l`
         * exactly when the link was broken, which is precisely backwards. Real
         * ls shows `l` for both, and the link's own size, which is the length
         * of its target. */
        NomStat st2;
        int alive = g_stat(path, &st2) == 0;
        g_puts("l");
        if (alive) g_putoct((unsigned)st2.mode, 4);
        else       g_puts("????");
        g_puts("  ");
        g_putpad(tl, 8);
        g_puts(label);
        g_puts(" -> "); g_puts(tgt);
        if (!alive) g_puts("   (DANGLING)");
        g_putln("");
        return;
    }
    if (g_stat(path, &st) == 0) {
        g_puts(st.kind == NOM_KIND_DIR ? "d" : "-");
        g_putoct((unsigned)st.mode, 4);
        g_puts("  ");
        g_putpad(st.size, 8);
    } else {
        /* stat failing on a name readdir just returned means a dangling
         * link -- worth showing, not worth hiding. */
        g_puts("l????         ?  ");
    }
    g_putln(label);
}

static void list_dir(const char *dir)
{
    for (int i = 0; i < 4096; i++) {
        if (g_readdir(dir, i, name) < 0) break;
        if (!show_all && name[0] == '.') continue;
        g_copy(full, dir, sizeof full);
        if (!g_streq(dir, "/")) g_cat(full, "/", sizeof full);
        g_cat(full, name, sizeof full);
        row(full, name);
    }
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    int n = g_argv(arg, v);
    g_argv_warn("ls");

    /* Flags. -l is what this listing already is, so it is honoured by being
     * the only form there is; -a and -d do real work. Anything else is
     * REJECTED by name rather than accepted and quietly ignored, because a
     * flag that does nothing is a worse answer than no flag at all. */
    int nop = 0;               /* operands, compacted into v[] in place */
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (v[i][0] != '-' || !v[i][1]) { v[nop++] = v[i]; continue; }
        for (const char *f = v[i] + 1; *f; f++) {
            if (*f == 'l') continue;          /* always long form */
            else if (*f == 'a') show_all = 1;
            else if (*f == 'd') dir_itself = 1;
            else {
                g_puts("ls: -"); { char c[2] = { *f, 0 }; g_puts(c); }
                g_putln(": not a flag this ls has");
                bad = 1;
            }
        }
    }
    if (bad) {
        g_putln("usage: ls [-l] [-a] [-d] [file|dir ...]");
        g_exit(2);
    }

    static const char *dirs[GARGS];
    int ndir = 0, nfile = 0, rc = 0;

    if (nop == 0) { dirs[ndir++] = "."; }
    for (int i = 0; i < nop; i++) {
        NomStat st;
        static char lk[192];
        /* A symlink operand is listed as itself, as `ls` does without -L.
         * (readlink refuses a buffer the target will not fit in, so this one
         * is the same size as the one row() prints from.) */
        int islink = g_readlink(v[i], lk, sizeof lk) > 0;
        if (!islink && g_stat(v[i], &st) != 0) {
            g_puts("ls: "); g_puts(v[i]); g_putln(": not found");
            rc = 1;
            continue;
        }
        if (!islink && !dir_itself && st.kind == NOM_KIND_DIR) dirs[ndir++] = v[i];
        else { row(v[i], v[i]); nfile++; }
    }

    /* Files first, then directories -- and a header on each directory only
     * when there is more than one thing being listed, which is what tells you
     * whose contents you are looking at. */
    int headers = (ndir + nfile) > 1;
    for (int i = 0; i < ndir; i++) {
        if (headers) {
            if (i || nfile) g_putln("");
            g_puts(dirs[i]); g_putln(":");
        }
        list_dir(dirs[i]);
    }
    g_exit(rc);
}

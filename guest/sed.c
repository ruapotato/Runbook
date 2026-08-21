/* /bin/sed — substitute in place.
 *
 *   sed s/old/new/ file        print the result
 *   sed -i s/old/new/ file     write it back
 *   sed -i /text/d file        delete every line containing `text`
 *
 * Both forms take any delimiter -- s|a|b|, |text|d, ,text,d -- and understand
 * \/ \n \t, because on this machine almost every pattern worth typing is a
 * path and a path is full of the obvious delimiter.
 *
 * Deliberately one expression and no regex: the job here is fixing a line in
 * a config from a rescue shell, which is a substitution and nothing more.
 * Anything cleverer would be a worse tool for that job and a much bigger one.
 */
#include "gsys.h"

static char arg[GARG_MAX], buf[65536], out[65536];

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);
    g_argv_warn("sed");

    int inplace = 0, ai = 0;
    if (n > 0 && g_streq(v[0], "-i")) { inplace = 1; ai = 1; }
    if (n < ai + 2) {
        g_putln("usage: sed [-i] s/old/new/ <file>   or   sed [-i] /text/d <file>");
        g_exit(1);
    }
    char *expr = v[ai], *file = v[ai + 1];

    /* /text/d -- delete every line containing `text`.
     *
     * Without this there was no way to remove a line from a file at all. A
     * playtester needed to delete one bad entry from /etc/fstab, had nothing
     * that could, and reinstalled the whole `filesystem` package to do it --
     * blowing away seven other files to get rid of one line. Deleting a line
     * is half of what anyone uses sed for on a broken machine. */
    /* ON THIS MACHINE EVERY INTERESTING LINE IS A PATH, and the delete form
     * could not express one. It stopped at the FIRST delimiter, so
     * `/usr/local/lib/d` was the pattern "usr" followed by rubbish, `\/` was
     * not understood, and `/usr.local.lib/d` matched nothing and said so as
     * if that were a result. Only a substring with no slashes in it worked.
     *
     * Three things, all of which the substitution form already had:
     *   - the closing delimiter is the one at the END, so an unescaped
     *     delimiter inside the pattern is just text: `/usr/local/lib/d`
     *   - any delimiter: `|text|d`, `,text,d`
     *   - \/ \n \t are understood, as in s///
     */
    u64 el = g_strlen(expr);
    char sepd = expr[0];
    int alnum = (sepd >= 'a' && sepd <= 'z') || (sepd >= 'A' && sepd <= 'Z') ||
                (sepd >= '0' && sepd <= '9');
    if (!alnum && el >= 3 && expr[el - 1] == 'd' && expr[el - 2] == sepd) {
        expr[el - 2] = 0;                  /* drop the closing <sep>d */
        char *pat = expr + 1;
        char *rp = pat, *wp = pat;
        while (*rp) {
            if (*rp == '\\' && rp[1]) {
                char ch = rp[1];
                rp += 2;
                if      (ch == 'n') *wp++ = '\n';
                else if (ch == 't') *wp++ = '\t';
                else                *wp++ = ch;   /* \/ -> /, \\ -> \ */
                continue;
            }
            *wp++ = *rp++;
        }
        *wp = 0;
        if (!*pat) { g_putln("sed: nothing to match"); g_exit(1); }

        i64 dlen = g_slurp(file, buf, sizeof buf);
        if (dlen < 0) { g_puts("sed: "); g_puts(file); g_putln(": cannot read"); g_exit(1); }
        u64 pl = g_strlen(pat), o2 = 0;
        int gone = 0;
        u64 i2 = 0;
        while (i2 < (u64)dlen) {
            u64 e2 = i2;
            while (e2 < (u64)dlen && buf[e2] != '\n') e2++;
            int match = 0;
            for (u64 q = i2; q + pl <= e2 && !match; q++) {
                u64 k2 = 0;
                while (k2 < pl && buf[q + k2] == pat[k2]) k2++;
                if (k2 == pl) match = 1;
            }
            if (match) gone++;
            else {
                for (u64 q = i2; q <= e2 && q < (u64)dlen && o2 + 1 < sizeof out; q++)
                    out[o2++] = buf[q];
                if (e2 >= (u64)dlen && o2 + 1 < sizeof out) out[o2++] = '\n';
            }
            i2 = e2 < (u64)dlen ? e2 + 1 : (u64)dlen;
        }
        out[o2] = 0;
        if (!inplace) { g_write(1, out, o2); g_exit(0); }
        /* `0 line(s) deleted` READ LIKE SUCCESS. It is the report of a command
         * that did nothing, and on a file the player is trying to repair that
         * is the difference between "fixed" and "your pattern is wrong". It
         * says which, names the pattern it looked for, and does not rewrite
         * the file it did not change. */
        if (!gone) {
            g_puts("sed: nothing matched "); g_puts(pat);
            g_puts(" in "); g_puts(file); g_putln(" -- no line was deleted.");
            g_putln("  the pattern is plain text, not a regular expression:");
            g_putln("  `.` is a full stop and matches only a full stop.");
            g_putln("  `grep <text> <file>` shows what would have matched.");
            g_exit(1);
        }
        int dfd = g_open(file, O_WRONLY | O_CREAT | O_TRUNC);
        if (dfd < 0) { g_puts("sed: "); g_puts(file); g_putln(": cannot write"); g_exit(1); }
        sysc(SYS_write, dfd, (i64)out, (i64)o2);
        g_close(dfd);
        g_puts("sed: "); g_putn(gone); g_puts(" line(s) deleted from ");
        g_putln(file);
        g_exit(0);
    }

    /* It looked like a delete and was not one. Say what the shape is, rather
     * than falling through to the substitution error, which does not mention
     * the form the player was reaching for. */
    if (expr[0] == '/') {
        g_putln("sed: expected /text/d -- the expression ends with the");
        g_putln("  delimiter and a `d`:  /text/d");
        g_putln("  any delimiter works: |text|d  ,text,d  -- and a `/` inside");
        g_putln("  the pattern is just a `/`: sed -i /usr/local/lib/d f");
        g_exit(1);
    }

    if (expr[0] != 's' || !expr[1]) {
        g_putln("sed: expected s<sep>old<sep>new<sep> or /text/d");
        g_putln("  any delimiter works: s/a/b/  s|a|b|  s,a,b,");
        g_putln("  and \\/ \\n \\t are understood in either half");
        g_exit(1);
    }
    /* ANY delimiter, as real sed allows. It was hardcoded to whatever
     * character followed the `s`, which sounds general and was not: the
     * moment the text contains a `/` -- and on this machine almost every
     * useful substitution is a file path -- there was no way to express it.
     * `sed "s|a|b|"` now works, and so does escaping the delimiter. */
    char sep = expr[1];
    char *from = expr + 2;

    /* Unescape in place: \<delim> becomes <delim>, \n a newline, \t a tab.
     * Without this `\/dev\/null` silently vanished and `\n` came out as the
     * letter n, which a playtester found the hard way. */
    char *rd = from, *wr = from;
    int part = 0;                      /* 0 = the FROM half, 1 = the TO half */
    char *to = 0;
    while (*rd) {
        if (*rd == '\\' && rd[1]) {
            char c = rd[1];
            rd += 2;
            if      (c == 'n') *wr++ = '\n';
            else if (c == 't') *wr++ = '\t';
            else               *wr++ = c;     /* \/ -> /, \\ -> \ */
            continue;
        }
        if (*rd == sep) {
            *wr++ = 0;
            rd++;
            if (part == 0) { to = wr; part = 1; }
            else break;                       /* trailing delimiter: done */
        } else {
            *wr++ = *rd++;
        }
    }
    *wr = 0;
    if (!to) { g_putln("sed: unterminated expression"); g_exit(1); }

    i64 len = g_slurp(file, buf, sizeof buf);
    if (len < 0) { g_puts("sed: "); g_puts(file); g_putln(": cannot read"); g_exit(1); }

    u64 fl = g_strlen(from), tl = g_strlen(to);
    if (fl == 0) { g_putln("sed: nothing to replace"); g_exit(1); }

    u64 o = 0;
    int hits = 0;
    for (u64 i = 0; i < (u64)len; ) {
        u64 k = 0;
        while (k < fl && i + k < (u64)len && buf[i + k] == from[k]) k++;
        if (k == fl) {
            for (u64 j = 0; j < tl && o + 1 < sizeof out; j++) out[o++] = to[j];
            i += fl;
            hits++;
        } else if (o + 1 < sizeof out) {
            out[o++] = buf[i++];
        } else break;
    }
    out[o] = 0;

    if (!inplace) { g_write(1, out, o); g_exit(0); }

    int fd = g_open(file, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { g_puts("sed: "); g_puts(file); g_putln(": cannot write"); g_exit(1); }
    sysc(SYS_write, fd, (i64)out, (i64)o);
    g_close(fd);
    g_puts("sed: ");
    g_putn(hits);
    g_puts(" replacement(s) in ");
    g_putln(file);
    g_exit(0);
}

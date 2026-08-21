/* /bin/grep — find a substring in a file. Plain text, no regex: the job here
 * is finding a line in a config, not parsing a language.
 *
 * `grep -c pat f` and `grep -i PAT f` both answered "cannot read", naming the
 * flag as though it were the missing file. The four flags that carry their
 * weight are implemented -- -i, -c, -n, -v -- and anything else is refused by
 * name. There is deliberately no -r and no -E: a flag that is accepted and
 * ignored would send a player looking for a match that grep never went to
 * find.
 *
 * It reads the file a line at a time rather than slurping it. The old one
 * stopped after 64 KB, so `grep oom /var/log/messages` searched the first
 * eighth of a 524 KB log and reported "no match" for the rest of it.
 */
#include "gsys.h"

static char arg[GARG_MAX], buf[4096], line[8192];
static char *v[GARGS];
static int fold, only_count, numbered, invert;

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int found_in(const char *hay, const char *needle)
{
    for (u64 i = 0; hay[i]; i++) {
        u64 k = 0;
        while (needle[k]) {
            char a = hay[i+k], b = needle[k];
            if (fold) { a = lower(a); b = lower(b); }
            if (a != b) break;
            k++;
        }
        if (!needle[k]) return 1;
    }
    return 0;
}

static i64 hits, lineno;

static void one_line(const char *pat, const char *label)
{
    lineno++;
    int m = found_in(line, pat);
    if (invert) m = !m;
    if (!m) return;
    hits++;
    if (only_count) return;
    if (label) { g_puts(label); g_puts(":"); }
    if (numbered) { g_putn(lineno); g_puts(":"); }
    g_putln(line);
}

static void scan(int fd, const char *pat, const char *label)
{
    u64 len = 0;
    for (;;) {
        i64 n = g_read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (i64 i = 0; i < n; i++) {
            if (buf[i] == '\n') { line[len] = 0; one_line(pat, label); len = 0; continue; }
            if (len + 1 < sizeof line) line[len++] = buf[i];
        }
    }
    if (len) { line[len] = 0; one_line(pat, label); }
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    int n = g_argv(arg, v);
    g_argv_warn("grep");

    int nop = 0;
    for (int i = 0; i < n; i++) {
        /* Only leading arguments are flags. Without this, `grep -- x` aside,
         * a pattern that begins with a dash could never be searched for --
         * and "-o" is a real thing to look for in a config. */
        if (nop > 0 || v[i][0] != '-' || !v[i][1]) { v[nop++] = v[i]; continue; }
        int bad = 0;
        for (const char *f = v[i] + 1; *f; f++) {
            if (*f == 'i') fold = 1;
            else if (*f == 'c') only_count = 1;
            else if (*f == 'n') numbered = 1;
            else if (*f == 'v') invert = 1;
            else bad = 1;
        }
        if (bad) {
            g_puts("grep: "); g_puts(v[i]); g_putln(": not a flag this grep has");
            g_putln("usage: grep [-i] [-c] [-n] [-v] <text> [file ...]");
            g_putln("  (plain substrings, not regular expressions)");
            g_exit(2);
        }
    }

    if (nop < 1) {
        g_putln("usage: grep [-i] [-c] [-n] [-v] <text> [file]   (reads stdin if no file)");
        g_exit(2);
    }
    const char *pat = v[0];

    int rc = 0;
    if (nop == 1) {
        scan(0, pat, 0);
        if (only_count) { g_putn(hits); g_puts("\n"); }
    } else {
        for (int i = 1; i < nop; i++) {
            int fd = g_open(v[i], O_RDONLY);
            if (fd < 0) {
                g_puts("grep: "); g_puts(v[i]); g_putln(": cannot read");
                rc = 2;
                continue;
            }
            i64 before = hits;
            lineno = 0;
            scan(fd, pat, nop > 2 ? v[i] : 0);
            g_close(fd);
            if (only_count) {
                if (nop > 2) { g_puts(v[i]); g_puts(":"); }
                g_putn(hits - before);
                g_puts("\n");
            }
        }
    }
    /* grep's exit status is its answer: 0 found, 1 not found, 2 broke. */
    g_exit(rc ? rc : (hits ? 0 : 1));
}

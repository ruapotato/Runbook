/* /bin/ed — the line editor.
 *
 * Every documented repair on this machine ends with "edit the file": man ip
 * says so, `netstat -F` says so, the shell's own `help` says so. There was no
 * editor. `sed -i` can change a line that already exists and `echo >>` can
 * add one at the end, and between them they cannot insert a line in the
 * middle, delete the third of four, or show you what you are about to change.
 * A playtester read three pages that told them to edit a file and found that
 * vi, ed, nano, edit and write all answered `command not found`.
 *
 * ed is the right editor for this machine and not a compromise. It is line
 * oriented, it needs no cursor addressing, it works over a serial console and
 * a socket, it is what a rescue medium has always carried, and it can be
 * driven by somebody who cannot see the screen.
 *
 * IT IS NOT INTERACTIVE, and it cannot be. A program here runs to completion
 * inside one command: there is no way for it to stop half way and ask for the
 * next line, because nothing will be typed until it has exited. So the ed
 * session is the argument list -- each argument is one line you would have
 * typed at ed -- and everything else is real ed, addressing and all:
 *
 *   ed /etc/resolv.conf ,n
 *   ed /etc/resolv.conf 2c "nameserver 10.0.2.3" . w
 *
 * The text of an a/i/c is the arguments that follow it, ended by a lone `.`,
 * exactly as it is ended at a real ed prompt.
 *
 * WHY NOT AUTOSAVE. `w` is a command here for the same reason it is one
 * everywhere: a buffer you have not written is a change you can still walk
 * away from, and the machine you are typing at is somebody's server. If the
 * script ends with the buffer changed and unwritten, ed says so in as many
 * words rather than leaving you to find out from the service that did not
 * come back.
 */
#include "gsys.h"

/* No allocator, so every ceiling is a fixed array and every one of them says
 * so when it is reached. A config file on this machine is tens of lines; the
 * log is half a megabyte and is not something anybody edits. */
#define ED_LINES 4096

static char arg[GARG_MAX];
static char text[65536];        /* the file, cut into NUL-terminated lines */
static char out[65536];         /* what `w` writes */
static char work[4096];         /* one line being substituted */
static char arena[16384];       /* lines that s/// built, which text has no room for */
static u64  apos;

static char *ln[ED_LINES];
static int  nl;                 /* how many lines are in the buffer */
static int  dot;                /* the current line, 1-based. 0 when empty */
static int  dirty;              /* the buffer differs from the file */
static const char *path;

/* Every refusal in this program says what went wrong, what the buffer looks
 * like now, and what to type next -- and then stops, without writing. A
 * half-applied edit script on a config file is worse than no edit at all. */
static void fail(const char *msg)
{
    g_puts("ed: ");
    g_putln(msg);
    g_puts("  the buffer has ");
    g_putn(nl);
    g_puts(" line(s) and ");
    g_putln(dirty ? "NOTHING was written." : "the file is untouched.");
    g_putln("  `ed <file> ,n` numbers every line, which is where to start.");
    g_exit(1);
}

/* Print one line, with or without its number. Numbers go in a fixed-width
 * column rather than after a tab: the terminal in the desktop does not expand
 * tabs, and a number that moves is a number nobody can line up. */
static void show(int i, int numbered)
{
    if (numbered) g_putpad(i, 5);
    g_putln(ln[i - 1]);
}

/* ------------------------------------------------------------ addressing */
/* One address: a number, `.` for the current line, `$` for the last. Returns
 * 0 when there is no address here, which is how a bare command gets its
 * default. */
static int addr1(char **p, int *v)
{
    char *s = *p;
    if (*s == '.')      { *v = dot; *p = s + 1; return 1; }
    if (*s == '$')      { *v = nl;  *p = s + 1; return 1; }
    if (*s >= '0' && *s <= '9') {
        int n = 0;
        while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
        *v = n; *p = s; return 1;
    }
    return 0;
}

/* A range. `,` on its own is the whole buffer, as it is in ed -- the spelling
 * everybody actually types for "all of it". */
static void addrs(char **p, int *a, int *b, int da, int db)
{
    int x, given = 0;
    *a = da; *b = db;
    if (addr1(p, &x)) { *a = x; *b = x; given = 1; }
    if (**p == ',') {
        (*p)++;
        /* A missing address either side of the comma is the end of the
         * buffer it is nearest, which is what makes `,` on its own mean all
         * of it. Testing `given` rather than comparing against the default
         * matters: `3,5p` with the current line already at 3 would otherwise
         * have looked like `,5p` and printed from the top. */
        if (!given) *a = 1;
        if (!addr1(p, &x)) x = nl;
        *b = x;
    }
}

static void need_range(int a, int b)
{
    if (nl == 0) fail("the buffer is empty -- there is no line to work on.");
    if (a < 1 || b > nl || a > b) {
        g_puts("ed: no line ");
        g_putn(a < 1 || a > nl ? a : b);
        g_puts(" -- the buffer has ");
        g_putn(nl);
        g_putln(" line(s).");
        g_putln("  `,n` numbers every one of them. `$` is the last.");
        g_exit(1);
    }
}

/* ------------------------------------------------------------- the lines */
/* Make room for `count` lines after position `at` (0 = before line 1). */
static void open_gap(int at, int count)
{
    if (nl + count > ED_LINES) {
        g_puts("ed: this buffer holds ");
        g_putn(ED_LINES);
        g_putln(" lines and that would be more.");
        g_putln("  nothing was written. edit the file in pieces, or `sed -i`");
        g_putln("  the change if it is one substitution.");
        g_exit(1);
    }
    for (int i = nl - 1; i >= at; i--) ln[i + count] = ln[i];
    nl += count;
}

static void del_range(int a, int b)
{
    int gone = b - a + 1;
    for (int i = b; i < nl; i++) ln[i - gone] = ln[i];
    nl -= gone;
}

/* Copy a line the substitution built into storage that outlives it. The
 * argument strings and the file buffer are both already stable; only s///
 * makes text that is not in either, so only s/// needs this. */
static char *keep(const char *s)
{
    u64 n = g_strlen(s);
    if (apos + n + 1 > sizeof arena) {
        g_putln("ed: no room left for another rewritten line.");
        g_putln("  nothing was written. `w` what you have to a scratch file");
        g_putln("  first: `w /tmp/part`, then carry on from there.");
        g_exit(1);
    }
    char *d = arena + apos;
    g_copy(d, s, sizeof arena - apos);
    apos += n + 1;
    return d;
}

/* ------------------------------------------------------------ s/old/new/ */
/* Any delimiter, as sed(1) here allows and for the same reason: on this
 * machine almost every string worth typing is a path, and a path is full of
 * the obvious delimiter. \<delim>, \t and \\ are understood. */
static void subst(char *p, int a, int b)
{
    char sep = *p++;
    if (!sep) fail("s needs a pattern: s/old/new/");
    char *from = p, *to = 0;
    char *rd = p, *wr = p;
    int part = 0;
    int global = 0;
    while (*rd) {
        if (*rd == '\\' && rd[1]) {
            char c = rd[1];
            rd += 2;
            if      (c == 't') *wr++ = '\t';
            else               *wr++ = c;      /* \/ -> /, \\ -> \ */
            continue;
        }
        if (*rd == sep) {
            *wr++ = 0;
            rd++;
            if (part == 0) { to = wr; part = 1; }
            else break;                        /* the closing delimiter */
        } else *wr++ = *rd++;
    }
    *wr = 0;
    if (!to) {
        g_putln("ed: unterminated s -- it takes three delimiters:  s/old/new/");
        g_putln("  any delimiter works, which is how a path is typed:");
        g_putln("  s|/usr/local|/opt|");
        g_exit(1);
    }
    while (*rd == 'g') { global = 1; rd++; }

    u64 fl = g_strlen(from), tl = g_strlen(to);
    if (!fl) fail("s has nothing to look for.");

    int hits = 0, last = dot;
    for (int i = a; i <= b; i++) {
        const char *src = ln[i - 1];
        u64 o = 0, k = 0;
        int did = 0;
        while (src[k]) {
            u64 m = 0;
            while (m < fl && src[k + m] == from[m]) m++;
            if (m == fl && (!did || global)) {
                for (u64 j = 0; j < tl && o + 1 < sizeof work; j++) work[o++] = to[j];
                k += fl;
                did = 1; hits++;
                continue;
            }
            if (o + 1 >= sizeof work) {
                g_puts("ed: line ");
                g_putn(i);
                g_putln(" would be longer than this editor can hold.");
                g_putln("  nothing was written. A shorter replacement, or");
                g_putln("  `sed -i`, which streams the file instead.");
                g_exit(1);
            }
            work[o++] = src[k++];
        }
        work[o] = 0;
        if (did) { ln[i - 1] = keep(work); last = i; dirty = 1; }
    }
    /* A substitution that matched nothing is the commonest way an edit
     * silently does not happen, and reporting it as success is how a player
     * comes to believe a file was repaired when it was not. */
    if (!hits) {
        g_puts("ed: nothing matched ");
        g_puts(from);
        g_putln(" -- no line was changed.");
        g_putln("  the pattern is plain text, not a regular expression.");
        g_putln("  `,n` prints the buffer with line numbers; `grep <text>");
        g_putln("  <file>` shows what would have matched.");
        g_exit(1);
    }
    dot = last;
    g_puts("ed: ");
    g_putn(hits);
    g_putln(" substitution(s).");
}

/* ------------------------------------------------------------------ w, q */
static void write_out(const char *where)
{
    u64 o = 0;
    for (int i = 0; i < nl; i++) {
        const char *s = ln[i];
        for (u64 j = 0; s[j]; j++) if (o + 2 < sizeof out) out[o++] = s[j];
        if (o + 2 < sizeof out) out[o++] = '\n';
    }
    int fd = g_open(where, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        g_puts("ed: "); g_puts(where); g_putln(": cannot write.");
        g_putln("  the buffer is intact and NOTHING was written. `df` and");
        g_putln("  `ls -l` say whether it is space or permission.");
        g_exit(1);
    }
    if (o && sysc(SYS_write, fd, (i64)out, (i64)o) < 0) {
        g_close(fd);
        g_putln("ed: the write failed part way -- no room on the disk.");
        g_putln("  `df`, `df -i` and `du /var` say where it went.");
        g_exit(1);
    }
    g_close(fd);
    dirty = 0;
    g_puts("ed: ");
    g_puts(where);
    g_puts(": ");
    g_putn(nl);
    g_puts(" line(s), ");
    g_putn((i64)o);
    g_putln(" bytes written.");
}

static void usage(void)
{
    g_putln("usage: ed <file> <ed command> ...      each argument is one line");
    g_putln("                                       you would type at ed");
    g_putln("  ,n              every line, numbered      3p     print line 3");
    g_putln("  2,5p            print a range            =      the line number");
    g_putln("  4d              delete line 4            2,5d   delete a range");
    g_putln("  s/old/new/      substitute on the current line, /g for all of");
    g_putln("                  them, any delimiter:  s|/usr|/opt|");
    g_putln("  2a <text> .     add after line 2. `i` before it, `c` instead of");
    g_putln("                  it. The text is the arguments up to a lone `.`");
    g_putln("  w               write it back            w <file>  write elsewhere");
    g_putln("  q               stop reading commands");
    g_putln("addresses are line numbers, `.` for the current line, `$` for the");
    g_putln("last, `a,b` for a range and `,` for all of it.");
    g_putln("");
    g_putln("  ed /etc/resolv.conf ,n");
    g_putln("  ed /etc/resolv.conf 2c \"nameserver 10.0.2.3\" . w");
    g_putln("  ed /etc/fstab '$a' \"/dev/sdb1 /data ext4 defaults 0 2\" . w");
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    static char *v[GARGS];
    int n = g_argv(arg, v);
    g_argv_warn("ed");

    if (n < 1) { usage(); g_exit(1); }
    path = v[0];

    /* Read the file in. A file that is not there is not an error: it is a new
     * file, which is how every config anybody adds gets written. Say which of
     * the two it was, because "0 lines" means different things. */
    i64 len = g_slurp(path, text, sizeof text);
    int had_nl = 1;
    if (len < 0) {
        g_puts("ed: "); g_puts(path);
        g_putln(": no such file -- starting an empty buffer.");
        g_putln("  `w` will create it. Nothing on the disk has changed yet.");
        len = 0;
    } else {
        if (len > 0 && text[len - 1] != '\n') had_nl = 0;
        u64 i = 0;
        while (i < (u64)len) {
            if (nl >= ED_LINES) {
                g_puts("ed: "); g_puts(path);
                g_puts(" has more than ");
                g_putn(ED_LINES);
                g_putln(" lines and this editor holds no more.");
                g_putln("  nothing was read past that. Use `sed -i` on a file");
                g_putln("  this size -- it streams and ed does not.");
                g_exit(1);
            }
            ln[nl++] = text + i;
            while (i < (u64)len && text[i] != '\n') i++;
            if (i < (u64)len) text[i++] = 0;
        }
        text[len] = 0;
    }
    dot = nl;

    if (n < 2) {
        /* ed with no script prints the size of what it opened, as ed always
         * has, and then the one thing a player needs to know next. */
        g_puts(path); g_puts(": "); g_putn(nl); g_puts(" line(s), ");
        g_putn(len); g_putln(" bytes.");
        if (!had_nl) g_putln("the last line has no newline; `w` will add one.");
        g_putln("");
        usage();
        g_exit(0);
    }
    if (!had_nl)
        g_putln("ed: the last line of this file has no newline; `w` adds one.");

    for (int i = 1; i < n; i++) {
        char *p = v[i];
        while (*p == ' ') p++;
        if (!*p) continue;

        int a, b;
        addrs(&p, &a, &b, dot, dot);
        char c = *p++;

        if (c == 'p' || c == 'n') {
            need_range(a, b);
            for (int k = a; k <= b; k++) show(k, c == 'n');
            dot = b;
            continue;
        }
        if (c == '=') {
            g_putn(a);
            g_putln("");
            continue;
        }
        if (c == 'd') {
            need_range(a, b);
            del_range(a, b);
            dirty = 1;
            dot = a <= nl ? a : nl;
            g_puts("ed: ");
            g_putn(b - a + 1);
            g_puts(" line(s) deleted, ");
            g_putn(nl);
            g_putln(" left. `w` to write it.");
            continue;
        }
        if (c == 's') { need_range(a, b); subst(p, a, b); continue; }
        if (c == 'w') {
            while (*p == ' ') p++;
            write_out(*p ? p : path);
            continue;
        }
        if (c == 'q') break;
        if (c == 'a' || c == 'i' || c == 'c') {
            /* The text is every argument up to a lone `.`, which is how the
             * text of an a/i/c is ended at a real ed prompt. Forgetting it is
             * the one mistake everybody makes here, so an unterminated one is
             * named rather than guessed at. */
            int first = i + 1, count = 0;
            while (first + count < n && !g_streq(v[first + count], ".")) count++;
            if (first + count >= n) {
                g_puts("ed: the text after `");
                char one[2] = { c, 0 };
                g_puts(one);
                g_putln("` is not ended.");
                g_putln("  a lone `.` on its own ends it, as it does at an ed");
                g_putln("  prompt:   ed <file> 2a \"a new line\" . w");
                g_putln("  nothing was written.");
                g_exit(1);
            }
            if (!count) {
                g_putln("ed: there is no text between that command and its `.`");
                g_putln("  -- nothing was added. Put the line between them.");
                g_exit(1);
            }
            /* `a` goes after the line, `i` before it, `c` replaces it. On an
             * empty buffer all three mean the same thing and none of them is
             * an error: that is how the first line of a new file is typed. */
            int at;
            if (c == 'c') {
                need_range(a, b);
                del_range(a, b);
                at = a - 1;
            } else if (nl == 0 || (a == 0 && b == 0)) {
                /* An empty buffer, or the address 0 that real ed takes for
                 * "before the first line" -- `0a` is how a line goes at the
                 * top of a file that already has one. */
                at = 0;
            } else {
                need_range(a, b);
                at = (c == 'a') ? b : a - 1;
            }
            open_gap(at, count);
            for (int k = 0; k < count; k++) ln[at + k] = v[first + k];
            dot = at + count;
            dirty = 1;
            g_puts("ed: ");
            g_putn(count);
            g_puts(c == 'c' ? " line(s) changed, " : " line(s) added, ");
            g_putn(nl);
            g_putln(" in the buffer. `w` to write it.");
            i = first + count;          /* skip the text and its `.` */
            continue;
        }

        g_puts("ed: ");
        g_puts(v[i]);
        g_putln(": not an ed command.");
        g_putln("  p n a i c d s w q = are all of them, and an address goes in");
        g_putln("  front:  3p  2,5d  ,n  $a  1i");
        g_putln("  nothing was written.");
        g_exit(1);
    }

    /* THE ONE THING THAT MAKES AN EDITOR FEEL LIKE IT LIED. The buffer is
     * changed, the script ended, and no `w` was in it -- so the file on the
     * disk is exactly what it was. Say it, and say what to type. */
    if (dirty) {
        g_puts("ed: ");
        g_puts(path);
        g_putln(" is NOT saved -- the buffer changed and no `w` was given.");
        g_putln("  the file on the disk is unchanged. Run the same line again");
        g_putln("  with `w` at the end of it.");
        g_exit(1);
    }
    g_exit(0);
}

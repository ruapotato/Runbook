/* /bin/sh — the shell.
 *
 * One command line per invocation, which is what an interactive session and a
 * remote connection both actually need. Builtins that change the process's own
 * state (cd, bind) act on the session, because the session IS a process and
 * its namespace and cwd persist between commands.
 *
 * Anything not a builtin is looked up in PATH and executed as a real program
 * on this machine. There is no magic: `ls` is /bin/ls, an rv64 binary, and if
 * it has been corrupted then `ls` fails the way a corrupted binary fails.
 */
#include "gsys.h"

static char line[GARG_MAX], cwd[256], tmp[512];

/* The `for` loop's variable. It shadows a real one for the length of the
 * loop, as it does in sh:
 *   for i in dev sys proc; do mount /$i /mnt/$i; done
 */
static char var_name[32], var_val[192];

/* VARIABLES.
 *
 * `X=5` answered "X=5: command not found" and `echo $X` printed nothing, and
 * both of those are things a person types in the first minute. The awkward
 * part is that /bin/sh here runs ONCE PER COMMAND LINE and exits, so a
 * variable kept in this program's memory would be gone before the next
 * prompt. They live on the session PROCESS instead, through SYS_setvar --
 * which is exactly where cd already keeps the working directory, and for
 * exactly the same reason. */
static int lookup(const char *nm, char *out, u64 cap)
{
    if (nm[0] && g_streq(nm, var_name)) { g_copy(out, var_val, cap); return 1; }
    return g_getvar(nm, out, cap) >= 0;
}

/* Substitute $name, ${name} and $?. An unset name expands to nothing,
 * exactly as it does in sh.
 *
 * SINGLE QUOTES AND A BACKSLASH STOP IT, as they do in every shell -- and
 * until ed(1) landed nothing on this machine noticed that they did not.
 * `$` is an ed address: `$a` appends after the last line, `$p` prints it, and
 * every spelling of them -- '$a', "$a", \$a -- came out as an empty word, so
 * the one address a player types without counting lines was unusable and the
 * error came from ed, about a command it had never been given. The same hole
 * hid `grep '$name' file` and any password with a dollar in it. Quoting has
 * to be honoured HERE, before expansion, because after it there is no way to
 * tell what was quoted. */
static void expand(const char *in, char *out, u64 cap)
{
    u64 o = 0;
    char q = 0;                  /* the quote we are inside, or 0 */
    for (u64 i = 0; in[i] && o + 1 < cap; ) {
        /* The quote marks themselves stay in the string: g_argv removes them
         * later, and it is the one that knows where a word ends. */
        if (!q && (in[i] == '\'' || in[i] == '"')) { q = in[i]; out[o++] = in[i++]; continue; }
        if (q && in[i] == q)                       { q = 0;     out[o++] = in[i++]; continue; }
        /* A backslash hands the next character through untouched, and leaves
         * the backslash for g_argv to eat. Not inside single quotes, where a
         * backslash is a backslash. */
        if (q != '\'' && in[i] == '\\' && in[i+1] && o + 2 < cap) {
            out[o++] = in[i++];
            out[o++] = in[i++];
            continue;
        }
        if (in[i] == '$' && in[i+1] && q != '\'') {
            u64 j = i + 1;
            char nm[32]; u64 k = 0;
            /* ${i} and $i both work; the braces matter when the name is
             * followed by a letter, as in /mnt/${i}x */
            int braced = (in[j] == '{');
            if (braced) j++;
            /* $? is the status of the last command, and it is the whole
             * reason a shell has a status at all. It is not a name, so it is
             * matched before the name loop. */
            if (in[j] == '?' && !braced) { nm[0] = '?'; nm[1] = 0; k = 1; j++; }
            else {
                while (in[j] && k + 1 < sizeof nm &&
                       ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z') ||
                        (in[j] >= '0' && in[j] <= '9') || in[j] == '_')) nm[k++] = in[j++];
                nm[k] = 0;
            }
            if (braced && in[j] == '}') j++;
            if (k) {
                char val[192];
                if (lookup(nm, val, sizeof val))
                    for (u64 q = 0; val[q] && o + 1 < cap; q++) out[o++] = val[q];
                i = j;
                continue;
            }
            /* A bare $ with nothing after it is a dollar sign. */
        }
        out[o++] = in[i++];
    }
    out[o] = 0;
}

static int run_line(char *cmd);

/* Run a ;-separated list of commands. Stops at the first failure, which is
 * what `set -e` gives you and what a boot script needs. */
static int is_for_impl(const char *s2)
{
    while (*s2 == ' ') s2++;
    return s2[0] == 'f' && s2[1] == 'o' && s2[2] == 'r' && s2[3] == ' ';
}

static int is_for(const char *s2) { return is_for_impl(s2); }

static const char *PATHDIRS[] = { "/bin", "/usr/bin", "/sbin", "/usr/sbin", 0 };

/* Where a command's output ends up. A pipeline already runs every stage with
 * its stdout captured; the only question left is what to do with the last
 * stage's, and answering that question with "a file" is the whole of `>`. */
#define OUT_CONSOLE (-1)
#define OUT_KEEP    (-2)      /* leave it in the pipe for $(...) to read */

static const char *resolve(const char *word)
{
    static char full[256];
    NomStat st;
    if (word[0] == '/' || word[0] == '.')
        return g_stat(word, &st) == 0 ? word : 0;
    for (int i = 0; PATHDIRS[i]; i++) {
        g_copy(full, PATHDIRS[i], sizeof full);
        g_cat(full, "/", sizeof full);
        g_cat(full, word, sizeof full);
        if (g_stat(full, &st) == 0) return full;
    }
    return 0;
}

/* Write, and SAY SO if it did not go. A redirect onto a full disk used to
 * create the file, write nothing, and report success -- which is the exact
 * failure `df` exists to explain, delivered as silence. */
static int wr(int fd, const char *b, u64 n)
{
    if (n && sysc(SYS_write, fd, (i64)b, (i64)n) < 0) {
        g_putln("sh: write failed -- no room on the disk, or the file is not");
        g_putln("  writable. `df`, `df -i` and `ls -l` say which.");
        return 1;
    }
    return 0;
}

/* Pour whatever the last stage wrote into a file descriptor. Chunked,
 * because a redirected `cat /var/log/messages` is half a megabyte and this
 * machine has no allocator to hold it in one piece. */
static int pipe_to_fd(int fd)
{
    static char chunk[2048];
    int bad = 0;
    for (;;) {
        i64 n = g_piperead(chunk, sizeof chunk);
        if (n <= 0) break;
        if (!bad) bad = wr(fd, chunk, (u64)n);
    }
    return bad;
}

/* a | b | c
 *
 * Each stage runs to completion and its output becomes the next stage's
 * input. There is no concurrency, which is right: these are filters, and a
 * filter that has not finished has nothing to say yet. */
static int run_pipeline(char *s2, int dst)
{
    int rc = 0;
    char *stage = s2;
    while (stage) {
        char *bar = stage;
        /* Quotes hide a pipe. Without this, `sed "s|a|b|" f` was torn into
         * three "pipeline stages" and the shell reported the middle of a
         * substitution as a command that could not be found -- which is why
         * an alternate sed delimiter appeared not to work at all. */
        {
            char q = 0;
            for (; *bar; bar++) {
                if (q)                        { if (*bar == q) q = 0; continue; }
                if (*bar == '"' || *bar == '\'') { q = *bar; continue; }
                if (*bar == '|') break;
            }
        }
        char save = *bar;
        *bar = 0;
        char *one = g_trim(stage);
        if (*one) {
            /* split verb from arguments for this stage */
            char *rest = one;
            while (*rest && *rest != ' ' && *rest != '\t') rest++;
            if (*rest) { *rest++ = 0; while (*rest == ' ') rest++; }
            const char *prog = resolve(one);
            if (!prog) {
                g_puts(one);
                g_putln(": command not found");
                return 127;
            }
            rc = (int)g_pipe(prog, rest);
        }
        *bar = save;
        stage = save ? bar + 1 : 0;
    }
    if (dst == OUT_CONSOLE)   g_pipeout();
    else if (dst >= 0)        pipe_to_fd(dst);
    /* OUT_KEEP: the caller is $(...) and will read it itself. */
    return rc;
}

/* `a && b` and `a || b`, with the short-circuit that makes them worth having.
 *
 * Neither was parsed. `echo a && echo b` printed the literal text
 * "a && echo b", because `&&` was never an operator and echo simply received
 * it as an argument -- which is the sort of thing that makes a shell feel
 * fake. They bind tighter than `;` here, as they do everywhere.
 *
 * Quotes are respected while scanning, so `grep "a && b" f` is one command. */
static int run_andor(char *s2)
{
    int rc = 0;
    while (*s2) {
        char *p = s2;
        char *op = 0;
        int is_and = 0;
        char q = 0;
        for (; *p; p++) {
            if (q)                      { if (*p == q) q = 0; continue; }
            if (*p == '"' || *p == '\'') { q = *p; continue; }
            if (p[0] == '&' && p[1] == '&') { op = p; is_and = 1; break; }
            if (p[0] == '|' && p[1] == '|') { op = p; is_and = 0; break; }
        }
        if (op) *op = 0;
        char *one = g_trim(s2);
        if (*one) rc = run_line(one);
        if (!op) return rc;
        s2 = op + 2;
        /* Short-circuit: && skips the rest on failure, || on success. */
        if (( is_and && rc != 0) || (!is_and && rc == 0)) {
            /* Skip to the next operator of the OPPOSITE persuasion, or the
             * end. Chained `a || b || c` must not run b AND c. */
            while (*s2) {
                char *n = s2, qq = 0;
                int found = 0;
                for (; *n; n++) {
                    if (qq)                        { if (*n == qq) qq = 0; continue; }
                    if (*n == '"' || *n == '\'')   { qq = *n; continue; }
                    if ((n[0] == '&' && n[1] == '&') ||
                        (n[0] == '|' && n[1] == '|')) { found = 1; break; }
                }
                if (!found) return rc;
                int next_and = (n[0] == '&');
                s2 = n + 2;
                if (( is_and && !next_and) || (!is_and && next_and)) break;
            }
        }
    }
    return rc;
}

static int run_list(char *s2)
{
    int rc = 0;
    /* A `for` owns the whole line, semicolons and all -- splitting first
     * would tear `for i in a b; do x; done` into three broken fragments. */
    if (is_for(s2)) return run_line(s2);
    while (*s2) {
        char *semi = s2, q = 0;
        for (; *semi; semi++) {
            if (q)                       { if (*semi == q) q = 0; continue; }
            if (*semi == '"' || *semi == '\'') { q = *semi; continue; }
            if (*semi == ';') break;
        }
        char save = *semi;
        *semi = 0;
        char *one = g_trim(s2);
        /* `;` runs the next command whatever happened to the last one. It
         * used to stop on failure, which is `&&` wearing a semicolon. */
        if (*one) rc = run_andor(one);
        *semi = save;
        s2 = save ? semi + 1 : semi;
    }
    return rc;
}


static int try_exec(const char *prog, const char *rest)
{
    NomStat st;
    if (prog[0] == '/' || prog[0] == '.') {
        if (g_stat(prog, &st) != 0) return -2;
        return (int)g_spawn(prog, rest);
    }
    for (int i = 0; PATHDIRS[i]; i++) {
        g_copy(tmp, PATHDIRS[i], sizeof tmp);
        g_cat(tmp, "/", sizeof tmp);
        g_cat(tmp, prog, sizeof tmp);
        if (g_stat(tmp, &st) == 0) return (int)g_spawn(tmp, rest);
    }
    return -2;
}

/* COMMAND SUBSTITUTION. `echo $(cat /etc/hostname)`
 *
 * This printed the literal text "(cat /etc/hostname)" -- the `$` was eaten as
 * an unset variable and the parentheses were just characters -- and backticks
 * did the same. It is the one shell feature that turns two commands into one
 * answer, and on this machine it is what lets `mount UUID=$(blkid ...)` be a
 * thing a person can type.
 *
 * The inner command runs through the ordinary pipeline machinery with its
 * output KEPT rather than flushed, then read back out of the pipe. Trailing
 * newlines go, embedded ones become spaces, exactly as sh does -- otherwise
 * `$(ls /etc)` would produce a command line with newlines in the middle of it.
 *
 * The INNERMOST substitution is done first and the pass repeats, so a nested
 * one works without this function ever calling itself. */
static char subbuf[GARG_MAX];

static int substitute(char *s2, u64 cap)
{
    for (int round = 0; round < 8; round++) {
        /* The LAST `$(` is the innermost one, so a nested substitution is
         * done from the inside out without this function recursing.
         * Backticks do not nest -- there is no way for them to -- so the
         * first one opens and the next one closes. */
        char *open = 0; int backtick = 0;
        for (char *q = s2; *q; q++)
            if (q[0] == '$' && q[1] == '(') open = q;
        if (!open) {
            for (char *q = s2; *q; q++)
                if (*q == '`') { open = q; backtick = 1; break; }
        }
        if (!open) return 0;
        char *body = open + (backtick ? 1 : 2);
        char *close = body;
        while (*close && *close != (backtick ? '`' : ')')) close++;
        if (!*close) {
            g_putln(backtick ? "sh: unmatched `" : "sh: unmatched $(");
            return 1;
        }

        static char inner[GARG_MAX];
        u64 bl = (u64)(close - body);
        if (bl >= sizeof inner) bl = sizeof inner - 1;
        for (u64 i = 0; i < bl; i++) inner[i] = body[i];
        inner[bl] = 0;

        run_pipeline(inner, OUT_KEEP);
        u64 got = 0;
        for (;;) {
            i64 n = g_piperead(subbuf + got, sizeof subbuf - 1 - got);
            if (n <= 0) break;
            got += (u64)n;
            if (got + 1 >= sizeof subbuf) break;
        }
        subbuf[got] = 0;
        while (got && (subbuf[got-1] == '\n' || subbuf[got-1] == '\r')) subbuf[--got] = 0;
        for (u64 i = 0; i < got; i++) if (subbuf[i] == '\n' || subbuf[i] == '\r') subbuf[i] = ' ';

        /* splice: [before][result][after] */
        static char rebuilt[GARG_MAX];
        u64 o = 0;
        for (char *q = s2; q < open && o + 1 < cap; q++) rebuilt[o++] = *q;
        for (u64 i = 0; i < got && o + 1 < cap; i++) rebuilt[o++] = subbuf[i];
        for (char *q = close + 1; *q && o + 1 < cap; q++) rebuilt[o++] = *q;
        rebuilt[o] = 0;
        g_copy(s2, rebuilt, cap);
    }
    return 0;
}

/* $? as the shell sees it: the last status, as text. */
static void set_status(int rc)
{
    char st[16];
    int i = 0, v = rc < 0 ? -rc : rc;
    char t[12];
    int k = 0;
    if (!v) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    if (rc < 0) st[i++] = '-';
    while (k) st[i++] = t[--k];
    st[i] = 0;
    g_setvar("?", st);
}

/* ------------------------------------------------------------- SCRIPTS --
 *
 * `sh path` runs the file at `path`, a line at a time, the way every shell on
 * earth does.
 *
 * NOMINAL's own README said "Nothing on this machine will run a shell script
 * from a file. If you want a sequence, type it" -- which was fine for a game
 * about repairing one broken box in an evening, and is not fine for this one.
 * RUNBOOK is about making yourself unnecessary (§3), and you cannot automate
 * anything with a shell that forgets everything the moment you press return.
 * This is the difference between a terminal and a tool.
 *
 * What a script is: lines. Blank ones and `#` comments are skipped, `#!` on
 * the first line is skipped because that is what a shebang is for, and every
 * other line goes through exactly the same run_list() an interactive line
 * does -- so for loops, pipelines, redirection, variables, && and || all work
 * in a file because they are not a separate implementation.
 *
 * The exit status is the last line's, which is what `set -e`-less shells do
 * and what a caller checking $? expects.
 */
#define SCRIPT_MAX 32768
static char script[SCRIPT_MAX];

static int run_script(const char *path)
{
    i64 n = g_slurp(path, script, sizeof script);
    if (n < 0) {
        g_puts("sh: cannot read ");
        g_putln(path);
        return 127;
    }
    int rc = 0;
    u64 i = 0;
    while (i < (u64)n) {
        u64 e = i;
        while (e < (u64)n && script[e] != '\n') e++;
        script[e] = 0;
        char *l = g_trim(script + i);
        if (*l && *l != '#') {
            rc = run_list(l);
            /* $? AFTER EVERY LINE, not just when the shell exits.
             *
             * Interactively, one command line is one `sh` process, and $?
             * survives because it is set on the way out. A script is MANY
             * lines in ONE process, so nothing set it between them and
             * `echo $?` inside a script printed nothing at all -- which
             * quietly removes the only way a shell script has of asking
             * whether the last thing worked. */
            set_status(rc);
        }
        i = e + 1;
    }
    return rc;
}

/* Is this one word, and does it name a file we could run? A script is
 * addressed by path, and anything with a space in it is a command line. */
static int is_script_path(const char *s)
{
    for (u64 i = 0; s[i]; i++)
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '|' || s[i] == '>' ||
            s[i] == '<' || s[i] == ';' || s[i] == '&' || s[i] == '$')
            return 0;
    NomStat st;
    if (g_stat(s, &st) != 0) return 0;
    return st.kind == NOM_KIND_FILE;
}

void _start(void)
{
    g_getarg(line, sizeof line);
    char *cmd = g_trim(line);
    if (!*cmd || *cmd == '#') g_exit(0);

    if (is_script_path(cmd)) {
        int src = run_script(cmd);
        g_exit(src);
    }

    int rc = run_list(cmd);
    /* $? outlives this process, because this process is one command line and
     * the person asking is on the next one. */
    set_status(rc);
    g_exit(rc);
}

/* Output redirection. `>` truncates, `>>` appends. Implemented by running the
 * command with stdout pointed at a file, which is what a shell does -- and it
 * is the only way to edit a file on this machine, so it is not a luxury:
 *     echo "nameserver 10.0.2.3" > /etc/resolv.conf
 */
static int redirect_fd = -1;

/* GLOBBING. `rm /tmp/*.tmp`
 *
 * A playtester met a machine with four hundred stale files in /tmp and no way
 * to delete them: no glob, no `rm -r`, no `find`. The fault was technically
 * solvable and practically not, and my solver never noticed because it walks
 * the directory in C. Fifteen minutes of their time went into a dead end that
 * a shell feature everybody expects would have closed in one line.
 *
 * Only * and ? and only in the last path component, which is where they
 * matter. An unmatched pattern is left alone, as sh does.
 *
 * WHAT IT USED TO DROP. The expansion itself was never capped, but everything
 * downstream of it was: eight argv slots and a 256-byte argument string. So
 * globbing every entry in /etc answered with the first eight of forty-one,
 * and the nine .conf files came back as eight -- silently, and on one ticket
 * the file it dropped was the broken one. The ceilings are much
 * higher now (GARGS, GARG_MAX) and glob_expand says so when it still hits
 * one, because a listing that stops early and does not admit it is worse than
 * no listing at all. */
static char globbuf[GARG_MAX];

static int glob_match(const char *pat, const char *nm)
{
    while (*pat && *nm) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (const char *q = nm; *q; q++)
                if (glob_match(pat, q)) return 1;
            return 0;
        }
        if (*pat != '?' && *pat != *nm) return 0;
        pat++; nm++;
    }
    while (*pat == '*') pat++;
    return !*pat && !*nm;
}

static int has_glob(const char *s2)
{
    for (; *s2; s2++) if (*s2 == '*' || *s2 == '?') return 1;
    return 0;
}

/* Expand every globbed word of `in` into `out`. */
static void glob_expand(const char *in, char *out, u64 cap)
{
    u64 o = 0;
    const char *p = in;
    while (*p) {
        while (*p == ' ' && o + 1 < cap) { out[o++] = *p++; }
        const char *w = p;
        while (*p && *p != ' ') p++;
        u64 wl = (u64)(p - w);
        static char word[512];
        if (wl >= sizeof word) wl = sizeof word - 1;
        for (u64 i = 0; i < wl; i++) word[i] = w[i];
        word[wl] = 0;

        if (!has_glob(word)) {
            for (u64 i = 0; i < wl && o + 1 < cap; i++) out[o++] = word[i];
            continue;
        }
        /* split into directory and pattern */
        static char dir[384], pat[128];
        int slash = -1;
        for (int i = (int)wl - 1; i >= 0; i--) if (word[i] == '/') { slash = i; break; }
        if (slash < 0) { g_copy(dir, ".", sizeof dir); g_copy(pat, word, sizeof pat); }
        else {
            u64 dl = (u64)(slash == 0 ? 1 : slash);
            for (u64 i = 0; i < dl && i < sizeof dir - 1; i++) dir[i] = word[i];
            dir[dl] = 0;
            g_copy(pat, word + slash + 1, sizeof pat);
        }

        int hits = 0, dropped = 0;
        static char nm[160];
        for (int i = 0; i < 4096; i++) {
            if (g_readdir(dir, i, nm) < 0) break;
            if (!glob_match(pat, nm)) continue;
            /* Does the WHOLE name fit? Emitting half of one and stopping
             * would hand the next program a filename that never existed. */
            u64 need = g_strlen(nm) + 1;
            if (slash >= 0) need += g_strlen(dir) + 1;
            if (o + need + 1 >= cap) { dropped++; continue; }
            if (hits) out[o++] = ' ';
            if (slash >= 0) {
                for (u64 k = 0; dir[k]; k++) out[o++] = dir[k];
                if (out[o-1] != '/') out[o++] = '/';
            }
            for (u64 k = 0; nm[k]; k++) out[o++] = nm[k];
            hits++;
        }
        if (dropped) {
            g_puts("sh: "); g_puts(word); g_puts(": matched ");
            g_putn(hits + dropped);
            g_puts(" names and only ");
            g_putn(hits);
            g_putln(" fit on one command line -- narrow the pattern");
        }
        if (!hits)      /* nothing matched: leave the pattern, as sh does */
            for (u64 i = 0; i < wl && o + 1 < cap; i++) out[o++] = word[i];
    }
    out[o < cap ? o : cap - 1] = 0;
}

static int run_line(char *cmd0)
{
    /* $(...) first, because its result is text that everything after this
     * should treat as if the person had typed it -- including the glob. */
    /* ON THE STACK, not in static storage. run_line calls itself -- a `for`
     * body goes back through run_list and straight back in here -- and a
     * static line buffer means the child overwrites the text its parent is
     * still walking through. This project has had that bug once already, in
     * `find`, where a directory came back out with one entry in it. Two
     * buffers of GARG_MAX is 8 KB of stack per frame, and the nesting here
     * is only ever a few deep. */
    char subst[GARG_MAX], expanded[GARG_MAX];

    g_copy(subst, cmd0, sizeof subst);
    if (g_contains(subst, "$(") || g_contains(subst, "`")) {
        if (substitute(subst, sizeof subst)) return 1;
        cmd0 = subst;
    }

    /* A `for` is parsed BEFORE expansion. Expanding first would substitute
     * $i while the loop variable is still unset, so the body would be built
     * with empty values and the loop would run the wrong command every time.
     * (That bug mounted / over everything.) */
    if (!is_for(cmd0)) {
        expand(cmd0, expanded, sizeof expanded);
    } else {
        g_copy(expanded, cmd0, sizeof expanded);
    }
    char *cmd = g_trim(expanded);
    if (!*cmd || *cmd == '#') return 0;

    /* After $ substitution and before anything is run. */
    if (!is_for(cmd) && has_glob(cmd)) {
        glob_expand(cmd, globbuf, sizeof globbuf);
        g_copy(expanded, globbuf, sizeof expanded);
        cmd = g_trim(expanded);
    }

    /* for NAME in A B C; do BODY; done
     *
     * Parsed here rather than by the session, because loop syntax is the
     * shell's business. The body is run once per word with NAME set. */
    if (cmd[0] == 'f' && cmd[1] == 'o' && cmd[2] == 'r' && cmd[3] == ' ') {
        char *p = cmd + 4;
        while (*p == ' ') p++;
        char *nm = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
        while (*p == ' ') p++;
        if (!(p[0] == 'i' && p[1] == 'n' && (p[2] == ' ' || p[2] == 0))) {
            g_putln("sh: for: expected `in`");
            return 1;
        }
        p += 2;
        char *words_start = p;
        /* the word list runs up to `do` (or a `;` before it) */
        char *dopos = 0;
        for (char *q = p; *q; q++) {
            if ((q == p || q[-1] == ' ' || q[-1] == ';') &&
                q[0] == 'd' && q[1] == 'o' && (q[2] == ' ' || q[2] == ';' || q[2] == 0)) {
                dopos = q; break;
            }
        }
        if (!dopos) { g_putln("sh: for: expected `do`"); return 1; }
        char *body = dopos + 2;
        dopos[0] = 0;
        /* trim a trailing ; from the word list */
        for (char *q = words_start; *q; q++) if (*q == ';') *q = ' ';
        /* the body runs up to a trailing `done` */
        u64 bl = g_strlen(body);
        while (bl && (body[bl-1] == ' ' || body[bl-1] == ';')) body[--bl] = 0;
        if (bl >= 4 && body[bl-4] == 'd' && body[bl-3] == 'o' &&
            body[bl-2] == 'n' && body[bl-1] == 'e') {
            body[bl-4] = 0;
        } else {
            g_putln("sh: for: expected `done`");
            return 1;
        }

        /* THE WORD LIST WAS 512 BYTES AND TRUNCATED IN SILENCE.
         * `for i in $(seq 1 300); do touch /tmp/f$i; done` created a hundred
         * and fifty-four files and said nothing, because 512 bytes of the
         * substitution is exactly seq's first 154 numbers. A loop that runs
         * half the times you asked and reports success is the worst kind of
         * bug in a game about believing what the machine tells you. It is
         * GARG_MAX now, like every other argument buffer, and it SAYS SO when
         * it still does not fit. */
        char *wv[GARGS];
        static char wcopy[GARG_MAX];
        if (g_strlen(words_start) >= sizeof wcopy) {
            g_putln("sh: for: the word list is longer than one command line --");
            g_putln("  loop over fewer things, or write them to a file.");
            return 1;
        }
        g_copy(wcopy, words_start, sizeof wcopy);
        int wn = g_argv(wcopy, wv);
        if (g_argv_over) {
            g_puts("sh: for: more than ");
            g_putn(GARGS);
            g_putln(" words to loop over -- narrow the list.");
            return 1;
        }
        static char bodycopy[1024];
        if (g_strlen(body) >= sizeof bodycopy) {
            g_putln("sh: for: the loop body is too long -- put it in a shorter");
            g_putln("  form, or run the steps one at a time.");
            return 1;
        }
        g_copy(var_name, nm, sizeof var_name);
        for (int i = 0; i < wn; i++) {
            g_copy(var_val, wv[i], sizeof var_val);
            g_copy(bodycopy, body, sizeof bodycopy);
            int rc = run_list(bodycopy);
            if (rc != 0) { var_name[0] = 0; return rc; }
        }
        var_name[0] = 0;
        return 0;
    }

    /* Pull off a trailing > or >> BEFORE anything else looks at the line, so
     * that a pipeline and a plain command reach the same code with the same
     * destination already decided. Quotes hide a >, as they hide a pipe. */
    int append = 0;
    char *redir = 0;
    {
        char q = 0;
        for (char *p = cmd; *p; p++) {
            if (q)                          { if (*p == q) q = 0; continue; }
            if (*p == '"' || *p == '\'')    { q = *p; continue; }
            if (*p != '>') continue;
            /* 2> is a different thing and this machine cannot honour it:
             * there is one output stream, and every program on it reports
             * its errors down the same pipe as its answers. Refusing is the
             * only honest option -- accepting it would make `ls /nope
             * 2>/dev/null` print the error anyway and look like a bug. */
            if (p > cmd && p[-1] == '2' && (p == cmd + 1 || p[-2] == ' ')) {
                g_putln("sh: 2>: this machine has one output stream, so there is");
                g_putln("  no separate stderr to send anywhere. Errors come out");
                g_putln("  with the answers; `> file` captures both.");
                return 1;
            }
            *p = 0;
            p++;
            if (*p == '>') { append = 1; p++; }
            while (*p == ' ') p++;
            redir = p;
            break;
        }
    }
    if (redir && !*redir) { g_putln("sh: > needs a file"); return 1; }
    if (redir) {
        redirect_fd = g_open(redir, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        if (redirect_fd < 0) {
            g_puts("sh: "); g_puts(redir); g_putln(": cannot write");
            return 1;
        }
    }
    cmd = g_trim(cmd);

    /* A pipeline is handled as a whole. Builtins do not pipe: cd and bind
     * change this process, and there is nothing to pipe them to. */
    for (char *q = cmd; *q; q++) {
        if (*q != '|') continue;
        int rc = run_pipeline(cmd, redirect_fd >= 0 ? redirect_fd : OUT_CONSOLE);
        if (redirect_fd >= 0) { g_close(redirect_fd); redirect_fd = -1; }
        return rc;   /* run_pipeline reports a failed write through wr() */
    }

    /* split verb from the rest, keeping the rest intact for the child */
    char *rest = cmd;
    while (*rest && *rest != ' ' && *rest != '\t') rest++;
    if (*rest) { *rest++ = 0; while (*rest == ' ') rest++; }

    /* `true` and `false` ARE THE EXIT STATUS AND NOTHING ELSE.
     *
     * They existed on disk as /bin/false containing the four bytes "#!false",
     * which is not an executable of any kind, so `false || echo fallback`
     * printed "/bin/false: not an ELF image" before the fallback ran. That is
     * an internal loader error leaking into the transcript of a command that
     * did exactly what it was asked to do -- and it made `true && ...` fail,
     * which is the opposite of what `true` is for. A word whose entire
     * meaning is a status is a builtin in every shell there is. */
    if (g_streq(cmd, "true"))  return 0;
    if (g_streq(cmd, "false")) return 1;

    if (g_streq(cmd, "cd")) {
        const char *to = *rest ? rest : "/";
        if (g_chdir(to) != 0) { g_puts("cd: "); g_puts(to); g_putln(": no such directory"); return 1; }
        return 0;
    }
    if (g_streq(cmd, "pwd")) {
        g_getcwd(cwd, sizeof cwd);
        /* A builtin redirects too. `pwd > f` writing to the console while
         * leaving an empty file behind would be the worst of both. */
        if (redirect_fd >= 0) {
            g_cat(cwd, "\n", sizeof cwd);
            int bad = wr(redirect_fd, cwd, g_strlen(cwd));
            g_close(redirect_fd);
            redirect_fd = -1;
            return bad;
        }
        g_putln(cwd);
        return 0;
    }
    if (g_streq(cmd, "bind")) {
        char *v[GARGS];
        int n = g_argv(rest, v);
        if (n < 2) { g_putln("usage: bind <target> <at>"); return 1; }
        if (g_bind(v[0], v[1]) != 0) { g_putln("bind: failed"); return 1; }
        return 0;
    }
    if (g_streq(cmd, "unbind")) {
        char *v[GARGS];
        if (g_argv(rest, v) < 1) { g_putln("usage: unbind <at>"); return 1; }
        if (g_unbind(v[0]) != 0) { g_putln("unbind: nothing bound there"); return 1; }
        return 0;
    }
    if (g_streq(cmd, "exit")) {
        /* Leaving a chroot is what `exit` means when you are in one -- that is
         * the flow the help text describes, and hanging up the connection
         * instead (which is what happened before) strands the player. */
        char cw[8] = "/";
        if (sysc(SYS_chroot, (i64)"//LEAVE", 0, 0) == 0) {
            g_putln("exit: left the chroot, back on the rescue medium");
            (void)cw;
            return 0;
        }
        g_putln("exit: not in a chroot (use `quit` to hang up)");
        return 0;
    }
    if (g_streq(cmd, "chroot")) {
        /* A builtin, not /bin/chroot, for the same reason cd is: it changes
         * THIS process's idea of where the root is, and a child that changed
         * its own and then exited would have accomplished nothing. */
        char *v[GARGS];
        if (g_argv(rest, v) < 1) { g_putln("usage: chroot <dir>"); return 1; }
        i64 crc = sysc(SYS_chroot, (i64)v[0], 0, 0);
        if (crc == -3) {
            /* Nothing is mounted there. This used to print the libc story
             * below -- a confident, plausible, completely fabricated
             * diagnosis pointing at a real fault class -- for a player whose
             * only mistake was skipping `mount`. */
            g_puts("chroot: "); g_puts(v[0]);
            g_putln(": nothing is mounted there, so there is nothing to");
            g_puts("  chroot into -- "); g_puts(v[0]);
            g_putln(" is an empty directory on this machine.");
            g_putln("  Mount the customer's disk on it first:");
            g_puts("      mount /dev/sda1 "); g_putln(v[0]);
            g_putln("  `mount` with no arguments prints what is mounted where.");
            return 1;
        }
        if (crc == -4) {
            g_puts("chroot: "); g_puts(v[0]);
            g_putln(": something is mounted there, but there is no /bin/sh");
            g_putln("  in it -- so there would be no shell to run and no way back");
            g_puts("  out. Check what you mounted: `ls "); g_puts(v[0]);
            g_putln("` and `mount`.");
            return 1;
        }
        if (crc == -2) {
            /* The shell in there cannot run. Refusing is the whole point:
             * entering anyway leaves you unable to run even `exit`. */
            g_puts("chroot: "); g_puts(v[0]);
            g_putln(": there is a /bin/sh in there, and it cannot run --");
            g_putln("  its libraries are missing or the wrong version, so every");
            g_putln("  command inside would fail, including the one to get out.");
            g_putln("  Work on the disk from OUT HERE instead:");
            g_puts("      pkg --root "); g_puts(v[0]); g_putln(" verify");
            g_putln("  takes the same verbs (verify, owns, diff, reinstall) and");
            g_putln("  never runs anything off the broken disk. `ldd` on a binary");
            g_putln("  under the mount point will tell you which library it is.");
            return 1;
        }
        if (crc != 0) {
            g_puts("chroot: "); g_puts(v[0]);
            g_putln(": not a directory (is anything mounted there?)");
            return 1;
        }
        g_puts("chroot: root is now "); g_putln(v[0]);
        return 0;
    }
    if (g_streq(cmd, "echo")) {
        /* Quotes are removed and -n is honoured, exactly as /bin/echo does --
         * this builtin shadows it, so fixing only the program fixed nothing.
         * `echo "udev.* /dev/null" >> f` used to write the quote marks into
         * the file, and there was no way to write a line containing a space
         * without them. When `echo >>` and `sed` were the only editors this
         * machine had, that decided whether a config file could be repaired
         * at all. /bin/ed exists now; this still has to be right. */
        static char ebuf[GARG_MAX];
        g_copy(ebuf, rest, sizeof ebuf);
        static char *ev[GARGS];
        int en = g_argv(ebuf, ev);
        g_argv_warn("echo");
        int ei = 0, enl = 1;
        if (en > 0 && g_streq(ev[0], "-n")) { enl = 0; ei = 1; }

        static char outb[GARG_MAX];
        u64 o = 0;
        for (; ei < en; ei++) {
            for (const char *q = ev[ei]; *q && o + 2 < sizeof outb; q++) outb[o++] = *q;
            if (ei + 1 < en && o + 2 < sizeof outb) outb[o++] = ' ';
        }
        if (enl && o + 1 < sizeof outb) outb[o++] = '\n';

        if (redirect_fd >= 0) {
            int bad = wr(redirect_fd, outb, o);
            g_close(redirect_fd);
            redirect_fd = -1;
            return bad;
        }
        g_write(1, outb, o);
        return 0;
    }
    if (g_streq(cmd, "help")) {
        g_putln("builtins:  cd  pwd  bind  unbind  echo  help");
        g_putln("           for i in a b c; do ... ; done      $i expands");
        g_putln("           NAME=value              a variable; $NAME expands it");
        g_putln("           $?                      the last command's status");
        g_putln("           $(command)              its output, as text");
        g_putln("           cmd > file              redirect (append with >>)");
        g_putln("           a | b | c               pipelines");
        g_putln("files:     ls cat cp mv rm touch mkdir grep head tail wc du");
        g_putln("           stat chmod sed find");
        /* THE EDITOR GOES ON ITS OWN LINE, because three manual pages end
         * with "edit the file" and a player who has not met this machine
         * cannot know what edits one. It was missing from here entirely. */
        g_putln("editing:   ed <file> ,n            the file, with line numbers");
        g_putln("           ed <file> 3c \"text\" . w   replace line 3 and save");
        g_putln("           man ed                  the rest of it: a i c d s w q");
        g_putln("           sed -i s/old/new/ <file>   one substitution, no");
        g_putln("                                      line numbers needed");
        g_putln("system:    ps ns mount umount chroot df uname whoami pkg");
        g_putln("network:   ip addr | link | route | neigh    what the stack holds");
        g_putln("           ping  traceroute  arp  ss  netstat  tcpdump");
        g_putln("           netstat -F               the firewall, and its drops");
        /* THE ONE THAT READS THE PAST, and the only reason it is on its own
         * line is that nobody thinks to look for it. Everything above
         * answers "what is the network doing now"; on a desk whose calls
         * broke up this morning the answer to that is "nothing", and the
         * player leaves believing the machine is healthy. It was. */
        g_putln("           voice                    calls this machine has");
        g_putln("                                    FINISHED, and why they broke");
        g_putln("           links <host>[/path]      try links wiki.nomnix.org");
        /* THE FOUR TOOLS A REPAIR ACTUALLY NEEDS, which this list left out.
         * A playtester fixed a mains-damaged filesystem here and said they
         * only knew to type `fsck` because the initrd had told them to --
         * every other route to it was reading the source. */
        g_putln("repair:    fsck <device>            a filesystem the power cut");
        g_putln("           pkg verify               what differs from what shipped");
        g_putln("           pkg diff <file>          shipped against what is there");
        g_putln("           pkg reinstall <package>  put it back, keeping a .pkgsave");
        g_putln("           blkid  ldd  svc  dmesg   disks, libraries, services, log");
        g_putln("");
        g_putln("the machine's own state is under /proc: try `cat /proc/self/ns`");
        g_putln("`man` on its own lists every manual this machine ships");
        return 0;
    }

    /* NAME=value.
     *
     * Checked here, after the builtins, so that nothing named like an
     * assignment can shadow one, and after expansion so that `X=$Y` and
     * `X=$(blkid ...)` both mean what they look like. It answered
     * "X=5: command not found" before, which is a shell telling you it is
     * not a shell. */
    {
        char *eq = 0;
        for (char *q = cmd; *q; q++) {
            if (*q == '=') { eq = q; break; }
            int ok = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                     (*q == '_') || (q != cmd && *q >= '0' && *q <= '9');
            if (!ok) break;
        }
        if (eq && eq != cmd) {
            *eq = 0;
            /* `X=one two` is an assignment of "one" and then a command, in
             * real sh. Here the whole remainder is the value, which is what
             * someone typing X=/mnt/etc/fstab actually wants and cannot be
             * confused with anything else. */
            static char val[192];
            g_copy(val, eq + 1, sizeof val);
            if (*rest) { g_cat(val, " ", sizeof val); g_cat(val, rest, sizeof val); }
            if (g_setvar(cmd, val) != 0) {
                g_puts("sh: "); g_puts(cmd);
                g_putln(": no room for another variable (16 is the limit)");
                return 1;
            }
            return 0;
        }
    }

    /* An ordinary program. With a redirect it is run through the pipe -- the
     * same machinery a pipeline uses -- and the capture is poured into the
     * file. This used to say "only `echo` can redirect at the moment", which
     * was an arbitrary restriction on the single most useful thing a shell
     * does -- and at the time `echo >>` and `sed` were the only editors on
     * the machine. */
    if (redirect_fd >= 0) {
        const char *prog = resolve(cmd);
        if (!prog) {
            g_close(redirect_fd); redirect_fd = -1;
            g_puts(cmd); g_putln(": command not found");
            return 127;
        }
        int rc = (int)g_pipe(prog, rest);
        int bad = pipe_to_fd(redirect_fd);
        g_close(redirect_fd);
        redirect_fd = -1;
        return bad ? 1 : (rc == 0 ? 0 : 1);
    }
    int rc = try_exec(cmd, rest);
    if (rc == -2) { g_puts(cmd); g_putln(": command not found"); return 127; }
    return rc == 0 ? 0 : 1;
}

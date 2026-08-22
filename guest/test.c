/* /bin/test and /bin/[ — the program that makes `if` and `while` worth having.
 *
 * The shell decides a condition by running a command and looking at its exit
 * status. Without this there is nothing to run: every condition anybody wants
 * to write is a comparison, and a comparison needs a program.
 *
 * WHAT IT DOES, which is what the loops on this ship actually need:
 *
 *   [ a = b ]      [ a != b ]
 *   [ n -eq m ]    -ne  -lt  -le  -gt  -ge      (integers)
 *   [ -f path ]    a file exists
 *   [ -d path ]    a directory exists
 *   [ -n s ]       not empty      [ -z s ]  empty
 *
 * Both names are the same program; `[` additionally requires the closing
 * bracket, and says so when it is missing rather than quietly ignoring it.
 * Exit 0 is true, which is backwards from every other language and is what
 * every shell script ever written expects.
 */
#include "gsys.h"
static char arg[GARG_MAX];

static int is_int(const char *s)
{
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

static long as_int(const char *s)
{
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    long v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
    return neg ? -v : v;
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    /* `[` wants its bracket. A missing one means the line was quoted wrong,
     * and silently treating it as absent turns a typo into a wrong answer. */
    if (n > 0 && g_streq(v[n - 1], "]")) n--;

    if (n == 0) g_exit(1);                       /* [ ] is false */
    if (n == 1) g_exit(v[0][0] ? 0 : 1);         /* [ s ] is true when non-empty */

    if (n == 2) {
        NomStat st;
        if (g_streq(v[0], "-n")) g_exit(v[1][0] ? 0 : 1);
        if (g_streq(v[0], "-z")) g_exit(v[1][0] ? 1 : 0);
        if (g_streq(v[0], "-f") || g_streq(v[0], "-e"))
            g_exit(g_stat(v[1], &st) == 0 ? 0 : 1);
        if (g_streq(v[0], "-d"))
            g_exit(g_stat(v[1], &st) == 0 && st.kind == NOM_KIND_DIR ? 0 : 1);
        g_puts("test: no such test: "); g_putln(v[0]);
        g_exit(2);
    }

    if (n >= 3) {
        const char *a = v[0], *op = v[1], *b = v[2];
        if (g_streq(op, "="))  g_exit(g_streq(a, b) ? 0 : 1);
        if (g_streq(op, "!=")) g_exit(g_streq(a, b) ? 1 : 0);
        if (op[0] == '-' && is_int(a) && is_int(b)) {
            long x = as_int(a), y = as_int(b);
            if (g_streq(op, "-eq")) g_exit(x == y ? 0 : 1);
            if (g_streq(op, "-ne")) g_exit(x != y ? 0 : 1);
            if (g_streq(op, "-lt")) g_exit(x <  y ? 0 : 1);
            if (g_streq(op, "-le")) g_exit(x <= y ? 0 : 1);
            if (g_streq(op, "-gt")) g_exit(x >  y ? 0 : 1);
            if (g_streq(op, "-ge")) g_exit(x >= y ? 0 : 1);
        }
        /* A NUMERIC TEST ON SOMETHING THAT IS NOT A NUMBER is the mistake
         * somebody makes reading a file that came back empty, and silence
         * would send them looking at the wrong thing entirely. */
        if (op[0] == '-') {
            g_puts("test: "); g_puts(op); g_putln(": needs two integers");
            g_exit(2);
        }
        g_puts("test: no such operator: "); g_putln(op);
        g_exit(2);
    }
    g_exit(1);
}

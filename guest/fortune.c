/* /usr/bin/fortune — one line of advice, chosen badly.
 *
 * The quotes are NOT in this binary. They are in /usr/share/fortunes, which is
 * a file the fortune package ships, which means `cat /usr/share/fortunes`
 * shows you the lot, `grep` finds one, `pkg verify fortune` notices when the
 * file is damaged, and a player who edits it gets their own fortunes. A joke
 * program that hides its data inside itself would be the one thing on this
 * machine you cannot look inside.
 *
 *   fortune           a line from /usr/share/fortunes
 *   fortune FILE      a line from FILE -- try ~nomowner/fortunes
 *
 * There is no clock on this machine and no random number generator, so the
 * "randomness" is the pid, stirred. Pids go up, so consecutive runs must not
 * give consecutive lines; multiplying by a large odd constant and folding the
 * high bits down is enough to make the order unguessable without pretending
 * to an entropy source we have not got.
 */
#include "gsys.h"

#define MAXLINES 256

static char arg[256];
static char body[16384];
static char flat[16384];
static char *line[MAXLINES];

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    const char *path = "/usr/share/fortunes";
    for (int i = 0; i < n; i++) {
        if (v[i][0] == '-' && v[i][1]) continue;   /* flags: accepted, ignored */
        path = v[i];
        break;
    }

    i64 len = g_slurp(path, body, sizeof body);
    if (len < 0) {
        g_puts("fortune: ");
        g_puts(path);
        g_putln(": cannot read");
        g_exit(1);
    }

    /* Split into fortunes. Blank lines and comments are not fortunes, so a
     * file can carry a header; and a line that begins with whitespace
     * CONTINUES the one before it, which is how ~nomowner's own list is
     * written and how anything longer than a terminal has to be written. The
     * result is flattened into one string per fortune, so a two-line entry
     * prints as one sentence rather than as two mystifying halves. */
    int nl = 0;
    u64 w = 0;
    char *p = body;
    while (*p && nl < MAXLINES) {
        char *s = p;
        int cont = (*s == ' ' || *s == '\t');
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        char *t = g_trim(s);
        if (!*t || *t == '#') continue;
        if (cont && nl > 0) {
            if (w + 1 < sizeof flat) flat[w - 1] = ' ';   /* over the NUL */
        } else {
            line[nl++] = &flat[w];
        }
        for (u64 i = 0; t[i] && w + 2 < sizeof flat; i++) flat[w++] = t[i];
        flat[w++] = 0;
    }
    if (nl == 0) {
        g_puts("fortune: ");
        g_puts(path);
        g_putln(": no fortunes in there");
        g_exit(1);
    }

    unsigned long h = (unsigned long)g_getpid();
    h ^= (unsigned long)len;
    h *= 6364136223846793005UL;
    h ^= h >> 33;
    h *= 1099511628211UL;
    h ^= h >> 29;

    g_putln(line[(int)(h % (unsigned long)nl)]);
    g_exit(0);
}

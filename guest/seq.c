/* /bin/seq — count, one number per line.
 *
 * This is not a toy. The shell here has `for` and `$(...)` but no arithmetic
 * and no way to say "do that four hundred times", so before seq existed there
 * was no way to WRITE the thing that exhausts a filesystem's inodes, and
 * therefore no way for a player to see for themselves why `df` and `df -i`
 * are two different questions:
 *
 *   for i in $(seq 1 400); do touch /tmp/$i; done
 *   df ; df -i
 *
 * It is also how you make a big file out of a small one, how you number
 * anything, and how you drive a loop over a fixed count rather than over a
 * glob that might match nothing.
 *
 *   seq LAST                 1 .. LAST
 *   seq FIRST LAST           FIRST .. LAST
 *   seq FIRST STEP LAST      FIRST, FIRST+STEP, ... not past LAST
 *
 * Counting DOWN needs a negative step, and g_num takes digits only, so a
 * leading '-' is handled here rather than pretended away. A step of zero is
 * refused: the alternative is a program that never returns on a machine with
 * no way to interrupt it.
 */
#include "gsys.h"

static char arg[256];

static int number(const char *s, i64 *out)
{
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!g_num(s, out)) return 0;
    if (neg) *out = -*out;
    return 1;
}

static void usage(void)
{
    g_putln("usage: seq [FIRST [STEP]] LAST");
    g_exit(1);
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    i64 first = 1, step = 1, last = 0;
    if (n == 1) {
        if (!number(v[0], &last)) usage();
    } else if (n == 2) {
        if (!number(v[0], &first) || !number(v[1], &last)) usage();
    } else if (n == 3) {
        if (!number(v[0], &first) || !number(v[1], &step) ||
            !number(v[2], &last)) usage();
    } else {
        usage();
    }

    if (step == 0) {
        g_putln("seq: a step of zero never arrives");
        g_exit(1);
    }

    /* An empty range prints nothing and succeeds, which is what makes
     * `for i in $(seq 1 0)` a loop that runs zero times instead of an error
     * the caller has to guard against. */
    for (i64 i = first; step > 0 ? i <= last : i >= last; i += step) {
        g_putn(i);
        g_puts("\n");
    }
    g_exit(0);
}

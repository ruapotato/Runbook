/* /bin/tail — the last ten lines, or the last N.
 *
 * This did not exist, and `head` did, so a player looking at a 524 KB
 * /var/log/messages could read only the WRONG END of it: the boot from March,
 * not the one that just failed. On a machine whose central fault is "what did
 * the log say when it died", that is the difference between a solvable ticket
 * and a guess.
 *
 * It does NOT slurp the file. A ring buffer keeps the last 16 KB as the file
 * streams past, so tail costs the same on a half-megabyte log as on a
 * ten-line config, and the answer is the real end of the file rather than the
 * end of the first 64 KB -- which is what a slurping tail would have quietly
 * given you.
 */
#include "gsys.h"

#define CAP 16384

static char arg[GARG_MAX], buf[4096], ring[CAP];
static char *v[GARGS];
static i64 want = 10;
static i64 tot;                  /* bytes seen; the ring holds the last CAP */

static char at(i64 i, i64 held)  /* logical byte i of what we kept */
{ return ring[(u64)((tot - held + i) % CAP)]; }

static void drain(int fd)
{
    tot = 0;
    for (;;) {
        i64 n = g_read(fd, buf, sizeof buf);
        if (n <= 0) break;
        for (i64 i = 0; i < n; i++) ring[(u64)((tot + i) % CAP)] = buf[i];
        tot += n;
    }

    i64 held = tot < CAP ? tot : CAP;
    if (!held) return;

    i64 i = held, lines = 0;
    if (at(i - 1, held) == '\n') i--;      /* the trailing newline ends a line */
    while (i > 0) {
        if (at(i - 1, held) == '\n') { if (++lines == want) break; }
        i--;
    }
    /* We ran off the front of what we kept AND there was more file behind it:
     * the answer is short and the player has to be told, because a tail that
     * silently returns nine of the ten lines you asked for is a tail you
     * cannot trust with the one line that mattered. */
    if (i == 0 && lines < want - 1 && tot > CAP) {
        g_puts("tail: only the last ");
        g_putn(CAP / 1024);
        g_putln(" KB of this file is kept -- these are the lines from there");
    }
    /* The answer is one contiguous run of the ring, unless it wraps, in which
     * case it is two. Never a syscall per byte. */
    u64 start = (u64)((tot - held + i) % CAP), len = (u64)(held - i);
    if (start + len <= CAP) g_write(1, ring + start, len);
    else {
        g_write(1, ring + start, CAP - start);
        g_write(1, ring, len - (CAP - start));
    }
}

void _start(void)
{
    g_getarg(arg, sizeof arg);
    int n = g_argv(arg, v);
    g_argv_warn("tail");

    int nop = 0;
    for (int k = 0; k < n; k++) {
        if (v[k][0] != '-' || !v[k][1]) { v[nop++] = v[k]; continue; }
        if (g_streq(v[k], "-n")) {
            if (k + 1 >= n || !g_num(v[k+1], &want)) {
                g_putln("tail: -n wants a number of lines");
                g_exit(2);
            }
            k++;
            continue;
        }
        if (g_streq(v[k], "-f")) {
            /* Nothing on this machine runs while you are typing at it, so a
             * follow would follow nothing. Saying that is better than
             * accepting the flag and sitting there. */
            g_putln("tail: -f: nothing runs while this shell is waiting, so");
            g_putln("  there would be nothing to follow. Run tail again.");
            g_exit(2);
        }
        const char *d = v[k] + 1;
        if (*d == 'n') d++;
        if (g_num(d, &want)) continue;
        g_puts("tail: "); g_puts(v[k]); g_putln(": not a flag this tail has");
        g_putln("usage: tail [-n N] [-N] [file ...]");
        g_exit(2);
    }

    if (nop == 0) { drain(0); g_exit(0); }

    int rc = 0;
    for (int k = 0; k < nop; k++) {
        int fd = g_open(v[k], O_RDONLY);
        if (fd < 0) {
            g_puts("tail: "); g_puts(v[k]); g_putln(": cannot read");
            rc = 1;
            continue;
        }
        if (nop > 1) {
            if (k) g_putln("");
            g_puts("==> "); g_puts(v[k]); g_putln(" <==");
        }
        drain(fd);
        g_close(fd);
    }
    g_exit(rc);
}

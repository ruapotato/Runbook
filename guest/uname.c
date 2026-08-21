/* /bin/uname — what am I running on.
 *
 * The version used to be a literal in this file, which made it a liar on the
 * one machine where the answer matters: boot a valid kernel image of the
 * WRONG version and the loader says "kernel 6.3.12 booting" while this said
 * 6.4.11. The fault catalogue recommended comparing uname against
 * /lib/modules to find exactly that fault -- against a tool that could not
 * participate in the comparison.
 *
 * It reads /proc/version now, which the loader writes with what it actually
 * loaded. A tool that reports a constant is not reporting.
 */
#include "gsys.h"
static char b[256];
static char v[128];

void _start(void)
{
    g_puts("NomnixOS ");
    if (g_slurp("/etc/hostname", b, sizeof b) > 0) g_puts(g_trim(b));
    else g_puts("(unknown)");

    /* "NomnixOS kernel 6.4.11 rv64" -> the third word. */
    const char *ver = 0;
    if (g_slurp("/proc/version", v, sizeof v) > 0) {
        char *t = g_trim(v);
        int sp = 0;
        for (char *p = t; *p; p++) {
            if (*p == ' ') {
                *p = 0;
                if (++sp == 2) { ver = p + 1; }
                else if (sp == 3) break;
                else continue;
            }
        }
        if (ver && !*ver) ver = 0;
    }
    g_puts(" ");
    g_puts(ver ? ver : "(unknown)");
    g_putln(" rv64 nominal");
    g_exit(0);
}

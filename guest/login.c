/* /sbin/login — the last thing a boot does. */
#include "gsys.h"
static char b[512];
void _start(void)
{
    if (g_slurp("/etc/issue", b, sizeof b) > 0) { g_puts("\n"); g_puts(g_trim(b)); g_puts("\n"); }
    if (g_slurp("/etc/hostname", b, sizeof b) > 0) { g_puts(g_trim(b)); g_putln(" login:"); }
    else g_putln("login:");
    g_exit(0);
}

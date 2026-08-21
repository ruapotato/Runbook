/* /usr/bin/open — start a graphical application from the shell.
 *
 * This is the seam between the operating system and the desktop, and it is
 * deliberately made of things you can look at. `open files` does three
 * checks, in the order a real display client does them:
 *
 *   1. is the display server running at all?      svc status nomde
 *   2. is there an application by that name?      /usr/share/applications
 *   3. can we reach the server's socket?          /run/nomde/requests
 *
 * Each of those can be broken independently, and each fails with a different
 * message -- which is the whole point. A desktop whose menu is painted on
 * cannot have a broken graphical stack; one that reads .desktop files and
 * talks through a socket can, and debugging it feels like debugging X11.
 *
 * The "socket" is a file because a file can be read. `cat /run/nomde/requests`
 * shows exactly what was asked for.
 */
#include "gsys.h"

static char buf[4096];
static char path[256];

static int nomde_running(void)
{
    static char pdir[96], pn[64], nm[128], st[32];
    for (int i = 0; i < 256; i++) {
        if (g_readdir("/proc", i, pn) < 0) break;
        g_copy(pdir, "/proc/", sizeof pdir);
        g_cat(pdir, pn, sizeof pdir);
        g_cat(pdir, "/status", sizeof pdir);
        if (g_slurp(pdir, buf, sizeof buf) < 0) continue;
        nm[0] = st[0] = 0;
        char *p = buf;
        while (*p) {
            char *nl = p; while (*nl && *nl != '\n') nl++;
            char save = *nl; *nl = 0;
            char *t = g_trim(p);
            if (t[0]=='n'&&t[1]=='a'&&t[2]=='m'&&t[3]=='e') {
                char *q = t + 4; while (*q==' '||*q==':') q++;
                g_copy(nm, q, sizeof nm);
            } else if (t[0]=='s'&&t[1]=='t'&&t[2]=='a'&&t[3]=='t') {
                char *q = t + 5; while (*q==' '||*q==':'||*q=='e') q++;
                g_copy(st, q, sizeof st);
            }
            *nl = save; p = *nl ? nl + 1 : nl;
        }
        if (g_contains(nm, "nomde") && g_contains(st, "running")) return 1;
    }
    return 0;
}

void _start(void)
{
    static char arg[256];
    g_getarg(arg, sizeof arg);
    char *v[GARGS];
    int n = g_argv(arg, v);

    if (n < 1) {
        g_putln("usage: open <application>");
        g_putln("");
        g_putln("installed applications:");
        static char nm[128];
        for (int i = 0; i < 64; i++) {
            if (g_readdir("/usr/share/applications", i, nm) < 0) break;
            if (!g_endswith(nm, ".desktop")) continue;
            g_puts("  ");
            for (u64 k = 0; nm[k] && nm[k] != '.'; k++) {
                char c[2] = { nm[k], 0 };
                g_puts(c);
            }
            g_puts("\n");
        }
        g_exit(1);
    }

    /* 1. the display server */
    if (!nomde_running()) {
        g_putln("open: cannot connect to the display server.");
        g_putln("  nomde is not running. `svc status nomde` says why, and it");
        g_putln("  only runs at runlevel 3 and 5 -- check `svc` first.");
        g_exit(1);
    }

    /* 2. the application */
    g_copy(path, "/usr/share/applications/", sizeof path);
    g_cat(path, v[0], sizeof path);
    g_cat(path, ".desktop", sizeof path);
    i64 dn = g_slurp(path, buf, sizeof buf);
    if (dn < 0) {
        g_puts("open: no application called "); g_putln(v[0]);
        g_putln("  `open` with no argument lists what is installed.");
        g_exit(1);
    }
    if (!g_contains(buf, "Exec=")) {
        g_puts("open: "); g_puts(path);
        g_putln(" has no Exec line -- the entry is damaged.");
        g_putln("  `pkg verify nomde` will say whether it was shipped that way.");
        g_exit(1);
    }

    /* 3. the socket */
    int fd = g_open("/run/nomde/requests", O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) {
        g_putln("open: cannot write to /run/nomde/requests.");
        g_putln("  the display server's socket is not there. is /run mounted,");
        g_putln("  and is the root filesystem writable?");
        g_exit(1);
    }
    sysc(SYS_write, fd, (i64)v[0], (i64)g_strlen(v[0]));
    sysc(SYS_write, fd, (i64)"\n", 1);
    g_close(fd);

    g_puts("open: asked nomde for "); g_putln(v[0]);
    g_exit(0);
}
